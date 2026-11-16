/* xz_session — 小智官方 WSS PTT 回合（ADR-014）。
 * 协议对齐 Bridge Go 实现（internal/xiaozhi/session.go）与
 * docs/XIAOZHI_DIRECT.md §1：hello → listen start(manual) → 裸 Opus
 * 二进制帧 → listen stop → stt/llm/tts 事件流 + Opus 音频下行。
 *
 * 线程模型：
 *   - 整轮同步跑在调用方 worker（静态栈 voice 任务，16KB .bss）：
 *     TLS 连接/WSS 握手/Opus 上行/下行解码播放都在同一任务里完成；
 *   - 不用 esp_websocket_client：其任务栈写死 4KB 且走内部 RAM，真机
 *     对话页稳态 largest_int ~7.6KB 时建不出来，建出来 TLS 握手也溢出
 *     （2026-11-16 实证，见 xz_session.c 头注释）。
 * 一次一回合（调用方语音任务本就串行）。 */
#ifndef XZ_SESSION_H
#define XZ_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* tts sentence_start 字幕增量（WS 事件线程调用；UI 侧自行异步+持锁） */
    void (*on_subtitle)(const char *text, void *user);
    void *user;
} xz_turn_cbs_t;

/* 直连就绪：身份已绑定且拿过 WSS 端点（未就绪调用方回落 Bridge 链） */
bool xz_direct_ready(void);

/* 同步跑一轮 PTT：pcm 16k/16bit/mono（frames 样本数）。下行音频由本模块
 * 经 audio_esp32_tts_* 流式播放（含起播阈值与播放收尾通知）。
 * 返回 0=回合完成（transcript/reply 填好） / -1=失败（err 填原因）。 */
int xz_voice_turn(const int16_t *pcm, int frames, const xz_turn_cbs_t *cbs,
                  char *transcript, int transcript_cap,
                  char *reply, int reply_cap,
                  char *err, size_t err_cap);

#endif /* XZ_SESSION_H */
