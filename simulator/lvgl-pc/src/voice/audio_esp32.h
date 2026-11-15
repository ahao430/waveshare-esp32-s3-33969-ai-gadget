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

/* 流式语音（?stream=1 帧协议）：没有 WAV 头，攒够 0.5s 就开播，
 * 后续 PCM 边收边播；stream_end 后播放任务在缓冲耗尽时收尾。 */
void audio_esp32_tts_begin_stream(void);
void audio_esp32_tts_write_pcm(const char *data, size_t len);
void audio_esp32_tts_stream_end(void);

#endif /* AUDIO_ESP32_H */
