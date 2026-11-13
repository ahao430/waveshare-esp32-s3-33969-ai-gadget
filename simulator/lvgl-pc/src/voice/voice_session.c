/* voice_session — 语音会话服务（PLAN §11.1 第二层：引擎无关）。
 *
 * 职责：
 *   - 会话状态机 Idle → Listening → Thinking → Speaking（+Error 自动回落）
 *   - 独占租约：同一时刻只授予一个语音引擎（ADR-003）
 *   - 音频硬件经 vs_audio_ops_t 抽象（模拟器=audio_mock；固件=I2S+ES8311/ES7210）
 *
 * 设计约束：
 *   - 不依赖 LVGL；推进由宿主调用 vs_tick()（定时器/主循环）。
 *   - 事件经 listener 回调通知 UI/引擎；回调在 vs_tick 调用方线程执行。
 *   - VAD/opus/连续对话为后续项（PLAN §11.2；Phase 5 真机），
 *     本骨架只实现 Push-to-Talk。
 */
#include "voice_session.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- 内部状态 ---- */

static const vs_audio_ops_t *g_ops;
static vs_listener_t g_listener;

static vs_state_t g_state = VS_IDLE;
static char g_owner[32];

static long g_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static long g_state_enter_ms;
static int g_think_ms = 1500;        /* Thinking 停留 */
static int g_speak_ms_per_char = 70; /* Speaking 每"字"推进 */
static int g_speak_pos;              /* 朗读推进位置（UTF-8 字符序号） */
static char g_reply[512];            /* 待朗读文本 */

#define PCM_FRAMES_PER_TICK 160 /* 16kHz × 10ms */
static long g_last_capture_ms;

/* ---- 内部辅助 ---- */

static void fire_state(vs_state_t st, const char *detail)
{
    if (g_listener.on_state) {
        g_listener.on_state(st, detail ? detail : "", g_listener.user);
    }
}

static void set_state(vs_state_t transition_to, const char *detail)
{
    if (g_state == transition_to) return;
    g_state = transition_to;
    g_state_enter_ms = g_now_ms();
    fire_state(transition_to, detail);
}

static size_t utf8_len(unsigned char c)
{
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF8) return 4;
    return 1;
}

/* 朗读可见前缀（UTF-8 字符粒度推进） */
static const char *speak_visible_prefix(int pos)
{
    static char buf[512];
    size_t chars = 0, bytes = 0;
    const char *s = g_reply;
    while (s[bytes] && chars < (size_t)pos) {
        bytes += utf8_len((unsigned char)s[bytes]);
        chars++;
    }
    if (bytes >= sizeof(buf)) bytes = sizeof(buf) - 1;
    memcpy(buf, s, bytes);
    buf[bytes] = '\0';
    return buf;
}

static void enter_idle(const char *why)
{
    if (g_ops) {
        if (g_ops->stop_capture) g_ops->stop_capture();
        if (g_ops->cancel_play) g_ops->cancel_play();
    }
    set_state(VS_IDLE, why);
}

/* ---- 对外 API ---- */

void vs_init(const vs_audio_ops_t *ops)
{
    g_ops = ops;
    g_reply[0] = '\0';
    set_state(VS_IDLE, NULL);
}

bool vs_acquire(const char *engine_id)
{
    if (g_owner[0] && strcmp(g_owner, engine_id) != 0) {
        fire_state(g_state, "busy"); /* 提示但不切状态 */
        return false;
    }
    snprintf(g_owner, sizeof(g_owner), "%s", engine_id);
    return true;
}

void vs_release(void)
{
    if (g_state != VS_IDLE) {
        enter_idle("released");
    }
    g_owner[0] = '\0';
}

const char *vs_owner(void)
{
    return g_owner[0] ? g_owner : NULL;
}

void vs_ptt_begin(void)
{
    if (!g_owner[0]) return;        /* 未持租约：no-op */
    if (g_state != VS_IDLE) return; /* 非 Idle：no-op */
    if (g_ops && g_ops->start_capture && g_ops->start_capture() != 0) {
        set_state(VS_ERROR, "capture_start_failed");
        return;
    }
    set_state(VS_LISTENING, NULL);
}

void vs_ptt_end(void)
{
    if (g_state != VS_LISTENING) return;
    if (g_ops && g_ops->stop_capture) g_ops->stop_capture();
    set_state(VS_THINKING, NULL);
}

void vs_notify_engine_done(void)
{
    if (g_state == VS_THINKING) {
        g_speak_pos = 0;
        set_state(VS_SPEAKING, NULL);
        if (g_ops && g_ops->play && g_reply[0]) {
            /* 通知音频层开始"播放"（mock no-op；real=TTS 管线启动）。
             * 完成经 vs_notify_play_done 可提前结束（打断）。 */
            if (g_ops->play(NULL, 0) != 0) {
                enter_idle("play_failed");
            }
        }
    }
}

void vs_set_reply(const char *text)
{
    snprintf(g_reply, sizeof(g_reply), "%s", text ? text : "");
}

void vs_notify_play_done(void)
{
    if (g_state == VS_SPEAKING) {
        enter_idle("play_done");
    }
}

void vs_set_listener(const vs_listener_t *l)
{
    if (l) {
        g_listener = *l;
    } else {
        memset(&g_listener, 0, sizeof(g_listener));
    }
}

void vs_set_mock_timing(int think_ms, int speak_ms_per_char)
{
    g_think_ms = think_ms;
    g_speak_ms_per_char = speak_ms_per_char;
}

vs_state_t vs_state(void)
{
    return g_state;
}

/* ---- tick：唯一推进点 ---- */

void vs_tick(void)
{
    long now = g_now_ms();
    long in_state = now - g_state_enter_ms;

    switch (g_state) {
    case VS_LISTENING:
        /* mock/real 采集由 ops 内部驱动；这里按 10ms 节奏喂假帧给 listener */
        if (g_ops == NULL && now - g_last_capture_ms >= 100) {
            g_last_capture_ms = now;
            if (g_listener.on_pcm) {
                int16_t pcm[PCM_FRAMES_PER_TICK] = {0}; /* 静音占位 */
                g_listener.on_pcm(pcm, PCM_FRAMES_PER_TICK, g_listener.user);
            }
        }
        break;

    case VS_THINKING:
        if (g_ops == NULL) {
            /* 纯状态机模式：固定时长后自动进 Speaking */
            if (in_state >= g_think_ms) {
                vs_notify_engine_done();
            }
        } else {
            /* 引擎模式：等 vs_notify_engine_done；超时兜底 15s */
            if (in_state >= 15000) {
                set_state(VS_ERROR, "engine_timeout");
            }
        }
        break;

    case VS_SPEAKING:
        /* 字幕进度始终推进（真实 TTS 场景 = 可视字幕；音频完成走
         * vs_notify_play_done 提前回 Idle）。文本读完也自动结束。 */
        if (in_state >= (long)g_speak_ms_per_char) {
            g_state_enter_ms = now;
            size_t bytes = 0, chars = 0;
            while (g_reply[bytes]) {
                bytes += utf8_len((unsigned char)g_reply[bytes]);
                chars++;
            }
            if ((size_t)g_speak_pos < chars) {
                g_speak_pos++;
                if (g_listener.on_speak_progress) {
                    g_listener.on_speak_progress(
                        speak_visible_prefix(g_speak_pos), g_listener.user);
                }
            } else {
                enter_idle("speak_done");
            }
        }
        break;

    case VS_ERROR:
        if (in_state >= 1200) {
            enter_idle("error_recovery");
        }
        break;

    case VS_IDLE:
    default:
        break;
    }
}
