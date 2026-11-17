/* music_backend_esp32.c — 固件音乐后端：esp_audio_codec 流式解码
 * + ES8311 输出（audio_esp32 media 通路）+ esp-dsp FFT 频谱。
 *
 * 任务与内存账（AGENTS 账本口径）：
 *   - 解码任务 xTaskCreateStatic 10KB .bss 静态栈（坑 #19 账：post-ui
 *     内部 free 70→60KB，>56KB 底线；启动后打 stack hwm 日志校准）
 *   - 解码器 heap 28KB 自动落 PSRAM（SPIRAM_MALLOC_ALWAYSINTERNAL=2048）
 *   - 读文件缓冲 16KB PSRAM；FFT 缓冲 4KB 静态
 *   - esp_codec_dev_write 阻塞背压天然节流，无环形缓冲
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_esp32.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "music_player.h"
#include "dsps_fft2r.h"
#include "esp_heap_caps.h"
#include <string.h>

/* ops 形状与 music_player.c 一致 */
typedef struct {
    int  (*open_track)(const char *path, int *duration_s, int *sample_rate,
                       char *fmt, int fmt_cap);
    int  (*start)(void);
    void (*pause)(void);
    void (*resume)(void);
    void (*stop)(void);
    void (*spectrum)(uint8_t bars[MUSIC_BARS]);
    void (*get_progress)(int *cur_s, int *total_s);
    int  (*poll_event)(void);
} music_backend_ops_t;

static void esp32_stop(void);

/* ---------------- 状态 ---------------- */

#define DEC_TASK_STACK  (16 * 1024)
#define READ_CHUNK      (16 * 1024)

/* 栈放 PSRAM（ALLOW_STACK_EXTERNAL=y 已开，LVGL 任务同款）：解码路径
 * fread(SDMMC)/I2S/FFT 不 disable flash cache，不踩坑 #14 断言；内部 RAM
 * 是坑 #19 红线资源，.bss 栈会把 Wi-Fi init 顶下线（2026-11-18 实证） */
static StackType_t *s_dec_stack; /* 16KB PSRAM 惰性 */
static StaticTask_t *s_dec_tcb;
static TaskHandle_t s_dec_task;
static volatile int s_run;       /* 任务存活 */
static volatile int s_pause;
static volatile int s_quit;
static volatile int s_eos;
static FILE *s_f;
static int s_rate = 44100, s_ch = 2;
static int s_total;              /* 总时长 s */
static volatile int s_cur_sec;
static int s_is_mp3;
static long s_wav_left;
static long s_file_size;
static int16_t *s_inbuf;         /* 16KB PSRAM 读缓冲 */
static int16_t *s_pcmout;        /* 解码输出缓冲（最大帧 2304x2ch） */
static uint8_t s_bars[MUSIC_BARS];
static float s_lvl[MUSIC_BARS];
/* FFT：512 点 fc32（esp-dsp），缓冲 PSRAM 惰性（~12KB，坑 #19 内部账） */
#define FFT_N 512
static float *s_fft_in;          /* 512 复数（4KB PSRAM） */
static int s_fft_pos;
static int s_fft_frames;

/* ---------------- MPEG 音频帧头解析（拿真实采样率/声道/码率） ---------------- */

static const int mp3_bitrate_v1l3[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112,
                                          128, 160, 192, 224, 256, 320 };
static const int mp3_sr_v1[3] = { 44100, 48000, 32000 };

/* 从文件头（跳过 ID3v2）找同步字 0xFFEx，解析帧头；0=OK */
static int mp3_probe_header(FILE *f, int *rate, int *ch, int *bitrate)
{
    uint8_t buf[2048];
    rewind(f);
    int n = (int)fread(buf, 1, sizeof(buf), f);
    int off = 0;
    if (n >= 10 && !memcmp(buf, "ID3", 3)) {
        int tagsz = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14) |
                    ((buf[8] & 0x7F) << 7) | buf[9];
        off = 10 + tagsz;
    }
    for (int i = off; i < n - 4; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
            int ver = (buf[i + 1] >> 3) & 0x03;
            int layer = (buf[i + 1] >> 1) & 0x03;
            int bri = buf[i + 2] >> 4;
            int sri = (buf[i + 2] >> 2) & 0x03;
            int pad = (buf[i + 2] >> 1) & 0x01;
            int mode = (buf[i + 3] >> 6) & 0x03;
            if (ver == 3 && layer == 0x01) { /* MPEG1 Layer3 */
                *rate = mp3_sr_v1[sri];
                *ch = (mode == 3) ? 1 : 2;
                *bitrate = mp3_bitrate_v1l3[bri] * 1000;
                (void)pad;
                fseek(f, i, SEEK_SET);
                return 0;
            }
            if (ver == 2 && layer == 0x01) { /* MPEG2 Layer3（低码率常见） */
                static const int br_v2[15] = { 0, 8, 16, 24, 32, 40, 48, 56,
                                               64, 80, 96, 112, 128, 144, 160 };
                static const int sr_v2[3] = { 22050, 24000, 16000 };
                *rate = sr_v2[sri];
                *ch = (mode == 3) ? 1 : 2;
                *bitrate = br_v2[bri] * 1000;
                fseek(f, i, SEEK_SET);
                return 0;
            }
        }
    }
    return -1;
}

/* ---------------- FFT 频谱（esp-dsp） ---------------- */

static void spectrum_init(void)
{
    dsps_fft2r_init_fc32(NULL, FFT_N);
}

/* PCM 追加 + 每 512 样本一拍：512 点 fc32 FFT → 16 bar 幅值快照 */
static void spectrum_feed(const int16_t *pcm, int frames)
{
    static float buf[FFT_N * 2];
    int total = frames;
    while (total > 0) {
        int n = total < FFT_N - s_fft_pos ? total : FFT_N - s_fft_pos;
        for (int i = 0; i < n; i++) {
            buf[s_fft_pos * 2] = pcm[i] / 32768.0f;
            buf[s_fft_pos * 2 + 1] = 0;
            s_fft_pos++;
        }
        total -= n;
        if (s_fft_pos < FFT_N) return;
        s_fft_pos = 0;
        /* 一拍：FFT → 16 组幅值 → 平滑衰减 */
        dsps_fft2r_fc32(buf, FFT_N);
        dsps_bit_rev_fc32(buf, FFT_N);
        for (int b = 0; b < MUSIC_BARS; b++) {
            float sum = 0;
            for (int k = 0; k < FFT_N / 2 / MUSIC_BARS; k++) {
                int bin = 2 + b * (FFT_N / 2 / MUSIC_BARS) + k;
                float re = buf[bin * 2];
                float im = buf[bin * 2 + 1];
                sum += re * re + im * im;
            }
            float mag = sqrtf(sum / (FFT_N / 2 / MUSIC_BARS));
            float v = mag * 400.0f;
            if (v > 255) v = 255;
            s_lvl[b] = s_lvl[b] * 0.7f + v * 0.3f;
            s_bars[b] = (uint8_t)s_lvl[b];
        }
    }
}

/* ---------------- 解码任务 ---------------- */

static void dec_task(void *arg)
{
    (void)arg;
    long pcm_frames = 0;
    esp_audio_simple_dec_handle_t dec = NULL;
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK ||
        !dec) {
        ESP_LOGE("music", "simple dec open failed");
        s_eos = 1;
        s_run = 0;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI("music", "dec task running hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    while (!s_quit) {
        if (s_pause) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!s_is_mp3) {
            /* WAV：PCM 直推（16bit 已由 riff parse 保证） */
            int n = (int)fread(s_pcmout, 1, 2304 * s_ch * 2, s_f);
            if (n <= 0) { s_eos = 1; break; }
            if (audio_esp32_media_write(s_pcmout, n) < 0) break;
            int frames = n / s_ch / 2;
            spectrum_feed(s_pcmout, frames);
            pcm_frames += frames;
            s_cur_sec = (int)(pcm_frames / (s_rate ? s_rate : 44100));
            continue;
        }
        int n = (int)fread(s_inbuf, 1, READ_CHUNK, s_f);
        if (n <= 0) {
            s_eos = 1;
            break;
        }
        uint8_t *in = (uint8_t *)s_inbuf;
        int left = n;
        while (left > 0 && !s_quit) {
            esp_audio_simple_dec_raw_t raw = {
                .buffer = in, .len = (uint32_t)left,
                .eos = (n < READ_CHUNK),
            };
            esp_audio_simple_dec_out_t out = {
                .buffer = (uint8_t *)s_pcmout,
                .len = 2304 * s_ch * 2,
                .needed_size = 0,
                .decoded_size = 0,
            };
            if (esp_audio_simple_dec_process(&dec, &raw, &out) !=
                ESP_AUDIO_ERR_OK) {
                break;
            }
            in += raw.consumed;
            left -= raw.consumed;
            if (out.decoded_size > 0) {
                int bw = audio_esp32_media_write(s_pcmout, (int)out.decoded_size);
                if (bw < 0) break; /* 输出通路释放（stop） */
                int frames = (int)out.decoded_size / s_ch / 2;
                spectrum_feed(s_pcmout, frames);
                pcm_frames += frames;
                s_cur_sec = (int)(pcm_frames / (s_rate ? s_rate : 44100));
            }
            if (raw.consumed <= 0) break; /* 防死循环 */
        }
        if (n < READ_CHUNK) break; /* 文件尾 */
    }
    esp_audio_simple_dec_close(dec);
    fclose(s_f);
    s_f = NULL;
    if (s_eos) ESP_LOGI("music", "track end cur=%ds", s_cur_sec);
    s_run = 0;
    vTaskDelete(NULL);
}

/* ---------------- ops 实现 ---------------- */

static int esp32_open_track(const char *path, int *duration_s, int *sample_rate,
                            char *fmt, int fmt_cap)
{
    (void)sample_rate;
    (void)fmt;
    (void)fmt_cap;
    esp32_stop();
    s_cur_sec = 0;
    s_is_mp3 = strlen(path) > 4 && strcasecmp(path + strlen(path) - 4, ".mp3") == 0;
    s_f = fopen(path, "rb");
    if (!s_f) {
        ESP_LOGE("music", "open %s failed", path);
        return -1;
    }
    fseek(s_f, 0, SEEK_END);
    s_file_size = ftell(s_f);
    if (s_is_mp3) {
        int bitrate = 128000;
        if (mp3_probe_header(s_f, &s_rate, &s_ch, &bitrate) != 0) {
            s_rate = 44100;
            s_ch = 2;
        }
        if (s_file_size > 0 && bitrate > 0)
            *duration_s = (int)((long long)s_file_size * 8 / bitrate);
    } else {
        int bits;
        long doff, dlen;
        if (music_riff_parse(s_f, &s_rate, &s_ch, &bits, &doff, &dlen) != 0) {
            ESP_LOGE("music", "unsupported wav %s", path);
            fclose(s_f);
            s_f = NULL;
            return -1;
        }
        fseek(s_f, doff, SEEK_SET);
        s_wav_left = dlen;
        *duration_s = (int)(dlen / (long)(s_rate * s_ch * 2));
    }
    s_total = *duration_s;
    /* 输出通路 owner + 读缓冲 */
    if (audio_esp32_media_acquire(s_rate, s_ch) != 0) {
        ESP_LOGE("music", "media acquire failed (busy?)");
        fclose(s_f);
        s_f = NULL;
        return -1;
    }
    if (!s_inbuf) {
        s_inbuf = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM);
        s_pcmout = heap_caps_malloc(2304 * 2 * 2, MALLOC_CAP_SPIRAM);
    }
    return 0;
}

static int esp32_start(void)
{
    if (s_run) return 0;
    s_quit = 0;
    s_pause = 0;
    s_eos = 0;
    s_fft_pos = 0;
    s_fft_frames = 0;
    s_run = 1;
    if (!s_dec_stack) {
        s_dec_stack = heap_caps_malloc(DEC_TASK_STACK, MALLOC_CAP_SPIRAM);
        s_dec_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_SPIRAM);
        memset(s_dec_stack, 0, DEC_TASK_STACK); /* 首次使用前清零（hwm 可读） */
    }
    s_dec_task = xTaskCreateStatic(dec_task, "music_dec",
                                   DEC_TASK_STACK / sizeof(StackType_t),
                                   NULL, 5, s_dec_stack, s_dec_tcb);
    if (!s_dec_task) {
        s_run = 0;
        return -1;
    }
    return 0;
}

static void esp32_pause(void) { s_pause = 1; }
static void esp32_resume(void) { s_pause = 0; }

static void esp32_stop(void)
{
    s_quit = 1;
    s_pause = 0;
    /* 任务在 media_write 阻塞处最多等到 I2S 消费一块（~几十 ms）自删 */
    int guard = 0;
    while (s_run && guard++ < 100) vTaskDelay(pdMS_TO_TICKS(10));
    s_eos = 0;
    if (s_f) {
        fclose(s_f);
        s_f = NULL;
    }
    audio_esp32_media_release();
    memset(s_bars, 0, sizeof(s_bars));
}

static void esp32_spectrum(uint8_t bars[MUSIC_BARS])
{
    memcpy(bars, s_bars, MUSIC_BARS);
}

static void esp32_get_progress(int *cur_s, int *total_s)
{
    if (cur_s) *cur_s = s_cur_sec;
    if (total_s) *total_s = s_total;
}

static int esp32_poll_event(void)
{
    int v = s_eos;
    s_eos = 0;
    return v;
}

int music_backend_esp32_ops(const music_backend_ops_t **ops)
{
    static const music_backend_ops_t ESP32_OPS = {
        esp32_open_track, esp32_start, esp32_pause, esp32_resume,
        esp32_stop, esp32_spectrum, esp32_get_progress, esp32_poll_event
    };
    *ops = &ESP32_OPS;
    return 0;
}
