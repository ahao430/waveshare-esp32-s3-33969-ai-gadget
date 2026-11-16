/* audio_esp32 — 实现，接口见 audio_esp32.h。
 *
 * 真机坑备忘（HARDWARE §2.3 bring-up 结论）：
 *   - ES7210 增益 42dB 削波 → 定 18dB；RMS 统计须 int64
 *   - esp_codec_dev_read/write 为阻塞调用 → 各自独立任务，不占 LVGL 线程
 *   - 播放完成 vs_notify_play_done 在播放任务线程触发 on_state 回调
 *     → ui_conversation 侧回调内的 UI 操作必须 ui_lock()（ui.h）
 */
#include "audio_esp32.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_session.h"

static const char *TAG = "audio_esp32";

/* 16kHz/16bit/mono；20ms 一块（320 帧） */
#define PCM_CHUNK_FRAMES 320
/* TTS 上限 1.5MB ≈ 48s 16k/16bit 语音（PSRAM）。
 * 流式一句一合成，~110 字中文实测 664KB，512KB 会截尾。 */
#define AUDIO_TTS_MAX_BYTES (1536 * 1024)

static esp_codec_dev_handle_t g_spk, g_mic;
static atomic_bool g_inited;

/* ---- 采集（PTT Listening 期间） ---- */

static atomic_bool g_cap_run;
static TaskHandle_t g_cap_task;

static void capture_task(void *arg)
{
    (void)arg;
    static int16_t chunk[PCM_CHUNK_FRAMES];
    unsigned long frames = 0;
    while (atomic_load(&g_cap_run)) {
        int rc = esp_codec_dev_read(g_mic, chunk, sizeof(chunk));
        if (rc != 0) {
            /* 读失败退避，避免死循环刷日志 */
            ESP_LOGW(TAG, "mic read rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        frames += PCM_CHUNK_FRAMES;
        vs_feed_pcm(chunk, PCM_CHUNK_FRAMES);
    }
    /* 采集量 0 = I2S 没出声（"按住说话没反应"的常见根因），必须留痕 */
    ESP_LOGI(TAG, "capture stop: %lu frames (%lu ms)",
             frames, frames * 1000UL / 16000UL);
    g_cap_task = NULL;
    vTaskDelete(NULL);
}

/* ---- 播放（TTS WAV 整段缓冲 → SPEAKING 时一次性播） ---- */

static char *g_tts;                 /* PSRAM；音频字节（WAV 头已剥） */
static volatile size_t g_tts_len;   /* 写侧：已写入字节数 */
static volatile size_t g_tts_head;  /* WAV 头解析状态（写侧） */
static atomic_bool g_play_run;
static TaskHandle_t g_play_task;
static atomic_bool g_tts_stream;    /* 流式：写完不代表结束（等 stream_end） */
static void play_task(void *arg);   /* 前置声明：流式写入要边收边起播 */
static atomic_bool g_tts_eof;       /* 写侧结束（流式） */

/* 流式播放阈值：攒够 1.5s 音频才开播，剩余边收边播。原 0.5s 对公网
 * Bridge（270ms RTT + 晚高峰抖动）不够——TCP 重传停顿超过已缓冲量就
 * 欠载卡顿（2026-11-16 用户反馈"声音好卡"）。1.5s 只推迟首声，内存
 * （同一 PSRAM 缓冲）与 CPU 零增加；小智直连（ADR-014）延迟低，届时
 * 可再调小。 */
#define TTS_START_BYTES (16000 * 3 / 2 * 2) /* 1.5s @16k/16bit/mono ≈ 48000B */

void audio_esp32_tts_reset(void)
{
    if (!atomic_load(&g_inited)) return;
    atomic_store(&g_play_run, false); /* 丢弃未播完的旧段 */
    atomic_store(&g_tts_stream, false);
    atomic_store(&g_tts_eof, true);
    g_tts_len = 0;
    g_tts_head = 0; /* WAV 头剥离游标归零 */
}

/* 流式开始：清空缓冲，标记"还会继续写"（播放任务等数据） */
void audio_esp32_tts_begin_stream(void)
{
    if (!atomic_load(&g_inited)) return;
    atomic_store(&g_play_run, false);
    atomic_store(&g_tts_stream, true);
    atomic_store(&g_tts_eof, false);
    g_tts_len = 0;
    g_tts_head = 44; /* 流式帧里没有 WAV 头，直接当已剥完 */
}

/* 流式结束：允许播放任务在缓冲耗尽后收尾 */
void audio_esp32_tts_stream_end(void)
{
    atomic_store(&g_tts_eof, true);
    /* 数据不足阈值时也要开播（短句） */
    if (g_tts_len > 0 && g_play_task == NULL) {
        atomic_store(&g_play_run, true);
        xTaskCreate(play_task, "vs_play", 4096, NULL, 5, &g_play_task);
    }
}

/* 流式 PCM 写入（无 WAV 头）：边收边播 */
void audio_esp32_tts_write_pcm(const char *data, size_t len)
{
    if (!atomic_load(&g_inited) || !data || len == 0) return;
    if (g_tts == NULL) {
        g_tts = malloc(AUDIO_TTS_MAX_BYTES);
        if (g_tts == NULL) {
            ESP_LOGE(TAG, "tts buffer alloc failed");
            return;
        }
    }
    size_t room = AUDIO_TTS_MAX_BYTES - g_tts_len;
    if (len > room) {
        if (room == 0) {
            ESP_LOGW(TAG, "tts stream full (%u), drop %u bytes",
                     (unsigned)AUDIO_TTS_MAX_BYTES, (unsigned)len);
            return;
        }
        len = room;
    }
    memcpy(g_tts + g_tts_len, data, len);
    g_tts_len += len;

    /* 攒够阈值就开播（首声提前）；状态机由 vs_notify_audio_started 推进 */
    if (g_play_task == NULL && g_tts_len >= TTS_START_BYTES) {
        atomic_store(&g_play_run, true);
        if (xTaskCreate(play_task, "vs_play", 4096, NULL, 5, &g_play_task) == pdPASS) {
            ESP_LOGI(TAG, "stream play start at %u bytes", (unsigned)g_tts_len);
            /* 状态机 THINKING → SPEAKING（不等全文）；回调在播放任务外的
             * 调用线程（HTTP worker）执行，UI 侧自带 ui_lock */
            vs_notify_audio_started();
        } else {
            atomic_store(&g_play_run, false);
        }
    }
}

void audio_esp32_tts_write(const char *data, size_t len)
{
    if (!atomic_load(&g_inited) || !data || len == 0) return;
    if (g_tts == NULL) {
        /* 惰性分配：≥16KB 自动落 PSRAM，跨请求复用不还堆 */
        g_tts = malloc(AUDIO_TTS_MAX_BYTES);
        if (g_tts == NULL) {
            ESP_LOGE(TAG, "tts buffer alloc failed");
            return;
        }
    }
    /* 剥 44B 标准 WAV 头（上游=Bridge wrapWav；分包边界安全：游标累计，
     * 攒满 44B 后开始收 PCM。真实 TTS 引擎变体接入时升级 chunk 解析） */
    if (g_tts_head < 44) {
        size_t need = 44 - g_tts_head;
        if (len < need) {
            g_tts_head += len;
            return; /* 本包全在头内 */
        }
        data += need;
        len -= need;
        g_tts_head = 44;
    }

    size_t room = AUDIO_TTS_MAX_BYTES - g_tts_len;
    if (len > room) {
        if (room == 0) return;
        len = room;
        ESP_LOGW(TAG, "tts truncated at %u bytes", (unsigned)AUDIO_TTS_MAX_BYTES);
    }
    memcpy(g_tts + g_tts_len, data, len);
    g_tts_len += len;
}

static void play_task(void *arg)
{
    (void)arg;
    /* 流式：边收边播——游标追到写游标就等新数据，直到写侧 EOF */
    size_t pos = 0;
    while (atomic_load(&g_play_run)) {
        size_t avail = g_tts_len;
        if (pos < avail) {
            size_t take = (avail - pos) < (size_t)(PCM_CHUNK_FRAMES * 2)
                              ? (avail - pos) : (size_t)(PCM_CHUNK_FRAMES * 2);
            if (esp_codec_dev_write(g_spk, (void *)(g_tts + pos), (int)take) != 0) {
                ESP_LOGW(TAG, "spk write failed");
                break;
            }
            pos += take;
            continue;
        }
        if (atomic_load(&g_tts_eof)) break; /* 写完了且播完 */
        vTaskDelay(pdMS_TO_TICKS(10));      /* 等流式下一帧 */
    }
    /* 完成通知：on_state(IDLE) 回调在播放线程 → UI 侧需 ui_lock */
    vs_notify_play_done();
    g_play_task = NULL;
    vTaskDelete(NULL);
}

/* 快速音量（状态栏弹窗）：0~100 直接映射 codec 音量 */
void audio_esp32_set_volume(int pct)
{
    if (!atomic_load(&g_inited) || !g_spk) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    esp_codec_dev_set_out_vol(g_spk, pct);
}

/* ---- UI 提示音（ui_sound 转发：番茄钟按键/倒计时结束） ---- */

#define BEEP_TWO_PI 6.2831853f

static TaskHandle_t g_beep_task;

/* 合成一段正弦分块直写 codec：16k 采样、首尾线性淡入淡出防爆音。
 * 相位由全局帧号直接算，块间无跳变。amp≈6000（点击）/9000（提醒）。 */
static void beep_write(int freq, int ms, int amp)
{
    static int16_t chunk[PCM_CHUNK_FRAMES]; /* 静态：单实例任务独占 */
    const int total = 16 * ms;              /* 16k → 每毫秒 16 帧 */
    for (int pos = 0; pos < total; pos += PCM_CHUNK_FRAMES) {
        int n = total - pos;
        if (n > PCM_CHUNK_FRAMES) n = PCM_CHUNK_FRAMES;
        for (int i = 0; i < n; i++) {
            int g = pos + i;
            float env = 1.0f;
            if (g < 16) env = (float)g / 16.0f;                   /* 1ms 淡入 */
            if (total - g < 80) env *= (float)(total - g) / 80.0f; /* 5ms 淡出 */
            chunk[i] = (int16_t)(sinf(BEEP_TWO_PI * (float)freq * (float)g / 16000.0f)
                                 * (float)amp * env);
        }
        if (esp_codec_dev_write(g_spk, chunk, (int)(n * sizeof(int16_t))) != 0) {
            return; /* 写失败（如正在被 TTS 独占异常）：放弃本声 */
        }
    }
}

static void beep_task(void *arg)
{
    int kind = (int)(intptr_t)arg;
    /* TTS 正在播：排队等它收尾（两路并发写 codec 会交错破音），
     * 最多等 10s——提醒宁可迟到/丢弃也不打断对话回复。 */
    for (int wait = 0; g_play_task != NULL && wait < 10000; wait += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (kind == AUDIO_BEEP_ALERT) {
        for (int i = 0; i < 3; i++) { /* 三连滴：阶段完成一听可辨 */
            beep_write(1046, 160, 9000); /* C6 */
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    } else {
        beep_write(1250, 70, 6000); /* 按键短滴 */
    }
    g_beep_task = NULL;
    vTaskDelete(NULL);
}

void audio_esp32_ui_beep(int kind)
{
    if (!atomic_load(&g_inited) || !g_spk) return;
    if (g_beep_task) return; /* 上一声还在播/在排队：合并 */
    if (xTaskCreate(beep_task, "ui_beep", 4096, (void *)(intptr_t)kind, 5,
                    &g_beep_task) != pdPASS) {
        g_beep_task = NULL;
    }
}

/* ---- vs_audio_ops_t ---- */

/* 采集任务常驻（init 时启动一次）：播放期间也必须持续读 mic——
 * 2026-11-16 连续对话实测，播完回复后的下一轮聆听全是静音
 * （maxamp=0~4，说话完全没进来）：按轮启停时 SPEAKING 期间无人读
 * ES7210，RX DMA 溢出后 esp_codec_dev_read 一直回零。常驻读取 +
 * vs_feed_pcm 只在 LISTENING 转发（非聆听期读到即丢弃，无拷贝）。 */
static int esp32_start_capture(void)
{
    return atomic_load(&g_inited) ? 0 : -1;
}

static void esp32_stop_capture(void)
{
    /* no-op：任务常驻，帧按状态丢弃 */
}

static int esp32_play(const int16_t *pcm, int frames)
{
    (void)pcm;
    (void)frames; /* 约定：real 模式 play(NULL,0)=TTS 缓冲开始播放 */
    if (!atomic_load(&g_inited)) return -1;
    if (g_tts_len == 0) {
        /* 无音频（如 mock TTS 缺席）：立即完成，文本进度照常走 */
        vs_notify_play_done();
        return 0;
    }
    if (g_play_task) return 0;
    atomic_store(&g_play_run, true);
    if (xTaskCreate(play_task, "vs_play", 4096, NULL, 5, &g_play_task) != pdPASS) {
        atomic_store(&g_play_run, false);
        return -1;
    }
    return 0;
}

static void esp32_cancel_play(void)
{
    atomic_store(&g_play_run, false);
}

static const vs_audio_ops_t g_esp32_ops = {
    .start_capture = esp32_start_capture,
    .stop_capture = esp32_stop_capture,
    .play = esp32_play,
    .cancel_play = esp32_cancel_play,
};

const vs_audio_ops_t *audio_esp32_ops(void)
{
    return &g_esp32_ops;
}

/* ---- 初始化（app_main 早期一次） ---- */

int audio_esp32_init(void)
{
    if (atomic_load(&g_inited)) return 0;

    ESP_ERROR_CHECK(bsp_audio_init(NULL));
    g_spk = bsp_audio_codec_speaker_init();
    g_mic = bsp_audio_codec_microphone_init();
    if (!g_spk || !g_mic) {
        ESP_LOGE(TAG, "codec init failed spk=%p mic=%p", (void *)g_spk, (void *)g_mic);
        return -1;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = 16000,
    };
    esp_codec_dev_open(g_spk, &fs);
    esp_codec_dev_open(g_mic, &fs);
    /* bring-up 定标：42dB 削波 → 18dB；音量初值 70（状态栏首次刷新会
     * 用 Web 配置的 volume 覆盖——ui_statusbar VOL_APPLY） */
    esp_codec_dev_set_out_vol(g_spk, 70);
    esp_codec_dev_set_in_gain(g_mic, 18.0);

    atomic_store(&g_inited, true);
    atomic_store(&g_cap_run, true);
    if (xTaskCreate(capture_task, "vs_cap", 4096, NULL, 5, &g_cap_task) != pdPASS) {
        ESP_LOGE(TAG, "capture task create failed");
        return -1;
    }
    ESP_LOGI(TAG, "audio ready: 16k/16bit/mono, spk vol 70, mic gain 18dB (capture resident)");
    return 0;
}
