/* audio_esp32 — 实现，接口见 audio_esp32.h。
 *
 * 真机坑备忘（HARDWARE §2.3 bring-up 结论）：
 *   - ES7210 增益 42dB 削波 → 定 18dB；RMS 统计须 int64
 *   - esp_codec_dev_read/write 为阻塞调用 → 各自独立任务，不占 LVGL 线程
 *   - 播放完成 vs_notify_play_done 在播放任务线程触发 on_state 回调
 *     → ui_conversation 侧回调内的 UI 操作必须 ui_lock()（ui.h）
 */
#include "audio_esp32.h"

#include <stdatomic.h>
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
/* TTS 上限 512KB ≈ 16s 语音（PSRAM） */
#define AUDIO_TTS_MAX_BYTES (512 * 1024)

static esp_codec_dev_handle_t g_spk, g_mic;
static atomic_bool g_inited;

/* ---- 采集（PTT Listening 期间） ---- */

static atomic_bool g_cap_run;
static TaskHandle_t g_cap_task;

static void capture_task(void *arg)
{
    (void)arg;
    static int16_t chunk[PCM_CHUNK_FRAMES];
    while (atomic_load(&g_cap_run)) {
        int rc = esp_codec_dev_read(g_mic, chunk, sizeof(chunk));
        if (rc != 0) {
            /* 读失败退避，避免死循环刷日志 */
            ESP_LOGW(TAG, "mic read rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        vs_feed_pcm(chunk, PCM_CHUNK_FRAMES);
    }
    g_cap_task = NULL;
    vTaskDelete(NULL);
}

/* ---- 播放（TTS WAV 整段缓冲 → SPEAKING 时一次性播） ---- */

static char *g_tts;                 /* PSRAM；音频字节（WAV 头已剥） */
static volatile size_t g_tts_len;   /* 有效字节数 */
static volatile size_t g_tts_head;  /* WAV 头解析状态（写侧） */
static atomic_bool g_play_run;
static TaskHandle_t g_play_task;

void audio_esp32_tts_reset(void)
{
    if (!atomic_load(&g_inited)) return;
    atomic_store(&g_play_run, false); /* 丢弃未播完的旧段 */
    g_tts_len = 0;
    g_tts_head = 0; /* WAV 头剥离游标归零 */
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
    const char *p = g_tts;
    size_t left = g_tts_len;
    while (atomic_load(&g_play_run) && left > 0) {
        size_t take = left < PCM_CHUNK_FRAMES * 2 ? left : PCM_CHUNK_FRAMES * 2;
        if (esp_codec_dev_write(g_spk, (void *)p, (int)take) != 0) {
            ESP_LOGW(TAG, "spk write failed");
            break;
        }
        p += take;
        left -= take;
    }
    /* 完成通知：on_state(IDLE) 回调在播放线程 → UI 侧需 ui_lock */
    vs_notify_play_done();
    g_play_task = NULL;
    vTaskDelete(NULL);
}

/* ---- vs_audio_ops_t ---- */

static int esp32_start_capture(void)
{
    if (!atomic_load(&g_inited)) return -1;
    if (g_cap_task) return 0; /* 上一轮任务未退（理论不到） */
    atomic_store(&g_cap_run, true);
    if (xTaskCreate(capture_task, "vs_cap", 4096, NULL, 5, &g_cap_task) != pdPASS) {
        atomic_store(&g_cap_run, false);
        return -1;
    }
    return 0;
}

static void esp32_stop_capture(void)
{
    atomic_store(&g_cap_run, false);
    /* 任务自行退出（下一次 esp_codec_dev_read 返回后）；
     * 尾部帧由 vs_feed_pcm 的状态检查丢弃，不 join（≤20ms 残留） */
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
    /* bring-up 定标：42dB 削波 → 18dB；音量 70 防破音 */
    esp_codec_dev_set_out_vol(g_spk, 70);
    esp_codec_dev_set_in_gain(g_mic, 18.0);

    atomic_store(&g_inited, true);
    ESP_LOGI(TAG, "audio ready: 16k/16bit/mono, spk vol 70, mic gain 18dB");
    return 0;
}
