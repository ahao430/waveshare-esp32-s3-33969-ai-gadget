/* voice_session — 语音会话服务（PLAN §11.1 第二层：引擎无关）。
 *
 * 职责：
 *   - 会话状态机 Idle → Listening → Thinking → Speaking（+Error）
 *   - 独占租约：同一时刻只授予一个语音引擎（PLAN ADR-003）
 *   - 音频硬件经 vs_audio_ops_t 抽象（模拟器=audio_mock；固件=I2S+ES8311/ES7210）
 *
 * 设计约束：
 *   - 本组件不依赖 LVGL/任何 UI；推进由宿主调用 vs_tick()（定时器/主循环）。
 *   - 事件经 listener 回调通知 UI/引擎；回调在 vs_tick 调用方线程执行。
 *   - VAD/opus/唤醒词为后续项（PLAN §11.2 连续对话模式；Phase 5 真机），
 *     本骨架只实现 Push-to-Talk。
 */
#ifndef VOICE_SESSION_H
#define VOICE_SESSION_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VS_IDLE = 0,      /* 休眠/待命 */
    VS_LISTENING,     /* 采集中（PTT 按住） */
    VS_THINKING,      /* 已提交，等引擎处理 */
    VS_SPEAKING,      /* 播放回复（TTS/文本进度） */
    VS_ERROR,         /* 音频/引擎错误；tick 自动回落 Idle */
} vs_state_t;

/* 音频硬件抽象。全部返回 0=成功，负值=错误。 */
typedef struct {
    /* 开始/停止采集（16kHz 16bit 单声道约定）。 */
    int (*start_capture)(void);
    void (*stop_capture)(void);
    /* 非阻塞播放；完成后宿主经 vs_notify_play_done() 告知。 */
    int (*play)(const int16_t *pcm, int frames);
    void (*cancel_play)(void);
} vs_audio_ops_t;

/* 状态监听（注册后由 vs_tick 在状态变化/朗读推进时回调）。 */
typedef struct {
    void (*on_state)(vs_state_t st, const char *detail, void *user);
    /* Speaking 时朗读进度：visible = 当前已"读出"的文本前缀。 */
    void (*on_speak_progress)(const char *visible, void *user);
    /* 采集帧回调（Listening 期间持续；UI 可做波形可视化） */
    void (*on_pcm)(const int16_t *pcm, int frames, void *user);
    void *user;
} vs_listener_t;

/* 初始化（ops 可为 NULL = 无音频的纯状态机，Thinking/Speaking 走模拟时长）。 */
void vs_init(const vs_audio_ops_t *ops);

/* 独占租约：成功返回 true；已被其他引擎占用返回 false（owner 不变）。 */
bool vs_acquire(const char *engine_id);
void vs_release(void);
const char *vs_owner(void);   /* NULL = 空闲 */

/* Push-to-Talk（需先 acquire）。非法状态调用是 no-op。 */
void vs_ptt_begin(void);
void vs_ptt_end(void);

/* 引擎处理完成（Thinking → Speaking）。Thinking 超时兜底 15s。 */
void vs_notify_engine_done(void);

/* Speaking 内容（ptt_end→Thinking→Speaking 时朗读；mock 链路无引擎，
 * 由宿主在 Thinking 期间经 vs_set_reply 设置假回复文本）。 */
void vs_set_reply(const char *text);

/* 宿主主动告知播放完成（audio ops 非阻塞约定）。 */
void vs_notify_play_done(void);

/* 主驱动：宿主定时器/主循环周期调用（建议 30~100ms）。
 * 推进 Thinking/Speaking 计时、驱动 mock 采集、触发 listener 回调。 */
void vs_tick(void);

vs_state_t vs_state(void);
void vs_set_listener(const vs_listener_t *l);

/* 模拟参数（演示用；真机接入后由 real ops 决定真实节奏） */
void vs_set_mock_timing(int think_ms, int speak_ms_per_char);

/* 模拟采集开关：开启后 Listening 期间由 vs_tick 喂伪语音帧（正弦+噪声
 * 包络，经 on_pcm 回调）——波形可视化/无音频环境演示用。真机 real ops
 * 自带真实采集，保持关闭。 */
void vs_set_mock_capture(bool enable);

/* 真机采集喂帧（audio_esp32 采集任务调用）：Listening 时转发 on_pcm
 * （回调在采集线程执行——消费方只做数据追加/标志写，勿碰 LVGL 对象），
 * 非 Listening 静默丢弃。 */
void vs_feed_pcm(const int16_t *pcm, int frames);

#endif /* VOICE_SESSION_H */
