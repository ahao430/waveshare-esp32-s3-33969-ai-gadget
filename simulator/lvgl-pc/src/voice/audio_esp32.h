/* audio_esp32 — vs_audio_ops_t 真机实现（仅固件编译；模拟器用 audio_mock）。
 * 采集=ES7210（I2S RX 任务，帧经 vs_feed_pcm）；播放=TTS WAV 整段缓冲，
 * SPEAKING 进入（ops->play）时起任务写 ES8311，播完 vs_notify_play_done。
 * 音频约定 16kHz/16bit/单声道（voice_session / PLAN §11.1）。 */
#ifndef AUDIO_ESP32_H
#define AUDIO_ESP32_H

#include <stddef.h>

#include "voice_session.h"

/* codec + I2S 初始化（app_main 早期调用一次；含音量/增益定标）。返回
 * 0=成功。内部持有 speaker/mic codec 句柄，重复调用 no-op。 */
int audio_esp32_init(void);

/* ops 装配（ui_conversation 的 vs_init 用） */
const vs_audio_ops_t *audio_esp32_ops(void);

/* TTS 音频流式写入（bridge_voice_interact 的 HTTP 读线程调用）：
 * 首包剥 WAV 头后只攒 PCM（当前上游=Bridge wrapWav 44B 标准头，真实
 * TTS 引擎接入时升级为 chunk 解析）；缓冲 PSRAM、上限
 * AUDIO_TTS_MAX_BYTES，超限截断（会话仍完成）。每次语音请求前
 * 调 audio_esp32_tts_reset()。 */
void audio_esp32_tts_reset(void);
void audio_esp32_tts_write(const char *data, size_t len);

/* 快速音量（状态栏弹窗/开机套用 Web 配置）；0~100 */
void audio_esp32_set_volume(int pct);

/* UI 提示音（番茄钟按键/倒计时结束，ui_sound 转发）：正弦合成直写
 * codec，起短任务播放（非阻塞，LVGL 线程可直接调）。TTS 播放中会
 * 排队等其收尾（最多 10s），不并发写 codec。 */
enum {
    AUDIO_BEEP_CLICK = 0, /* 按键反馈：单声短滴 70ms */
    AUDIO_BEEP_ALERT = 1, /* 倒计时结束：三连滴（C6 160ms×3） */
};
void audio_esp32_ui_beep(int kind);

/* 流式语音（?stream=1 帧协议）：没有 WAV 头，攒够 0.5s 就开播，
 * 后续 PCM 边收边播；stream_end 后播放任务在缓冲耗尽时收尾。 */
void audio_esp32_tts_begin_stream(void);
void audio_esp32_tts_write_pcm(const char *data, size_t len);
void audio_esp32_tts_stream_end(void);

/* ---------------- 音乐媒体通路（音乐播放器专用，owner 仲裁） ----------------
 * TX 同一时刻只允许一个写者（beep 等待 TTS 的先例是模块内临时约定），
 * 音乐引入第三方写者后升级为显式 owner 标志：音乐 acquire 期间
 * tts_begin_stream/tts_write_pcm/beep 一律拒绝。采样率经 set_fs 切换
 * （ES8311 支持 8-96k），release 回 16k/mono 交还语音。 */
int audio_esp32_media_acquire(int sample_rate, int channels);
int audio_esp32_media_write(const void *pcm, int len); /* 阻塞写（背压节流） */
void audio_esp32_media_release(void);
int audio_esp32_media_active(void);   /* 1=音乐占用输出（PTT/UI 查询） */

#endif /* AUDIO_ESP32_H */
