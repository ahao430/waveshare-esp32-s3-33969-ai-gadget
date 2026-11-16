/* mock 实现：复用 mock_data 的伪随机数值（设备本地状态恒由 mock 提供）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "data_source_internal.h"
#include "mock_data.h"

static void mock_init(void)
{
    (void)mock_state(); /* 触发内部初值（重置时间/更新时间） */
}

/* 设备运行配置下发：mock 恒 15 分钟（与 PLAN §6.3 默认一致），
 * 亮度/音量不下发（保持本地值——mock 无持久化语义）。 */
static int mock_device_config_fetch(ds_device_config_t *out)
{
    if (!out) return -1;
    out->sub_refresh_interval_s = 900;
    return 0;
}

static bool mock_ds_refresh(gadget_state_t *st)
{
    mock_refresh(); /* mock_data 的同名函数：伪随机刷新底层状态 */
    const mock_state_t *m = mock_state();
    st->zhipu_used_5h    = m->zhipu_used_5h;
    st->zhipu_used_week  = m->zhipu_used_week;
    st->zhipu_reset_5h   = m->zhipu_reset_5h;
    st->zhipu_reset_week = m->zhipu_reset_week;
    if (strlen(m->zhipu_plan) < sizeof(st->zhipu_plan)) {
        strcpy(st->zhipu_plan, m->zhipu_plan);
    }
    st->kimi_used_5h   = m->kimi_used_5h;
    st->kimi_used_week = m->kimi_used_week;
    st->updated_unix   = m->updated_unix;

    /* mock 固定 3 条演示订阅（演示三种卡形态；空态语义属于 bridge 无配置场景）：
     * [0] 智谱 v1：仅 5h 窗口、无周限（真实套餐形态），可预热；
     * [1] DeepSeek：余额型按量计费；[2] Kimi：5h+周 完整窗口形态。 */
    st->sub_count = 3;
    st->subs[0] = (ds_subscription_t){0};
    st->subs[0].used_5h   = st->zhipu_used_5h;
    st->subs[0].reset_5h  = st->zhipu_reset_5h;
    st->subs[0].win_5h_s  = 18000; /* v1 套餐无周限：week_s=0 */
    st->subs[0].can_prewarm = true;
    snprintf(st->subs[0].name, sizeof(st->subs[0].name), "智谱 Coding Plan");
    snprintf(st->subs[0].plan, sizeof(st->subs[0].plan), "GLM Coding Lite · 12月11日到期");
    st->subs[0].valid = true;

    st->subs[1] = (ds_subscription_t){0};
    snprintf(st->subs[1].name, sizeof(st->subs[1].name), "DeepSeek");
    snprintf(st->subs[1].plan, sizeof(st->subs[1].plan), "官方余额（按量计费）");
    snprintf(st->subs[1].balance, sizeof(st->subs[1].balance), "110.35");
    st->subs[1].valid = true;

    st->subs[2] = (ds_subscription_t){0};
    st->subs[2].used_5h   = 30;
    st->subs[2].used_week = 62;
    st->subs[2].reset_5h  = st->zhipu_reset_5h;
    st->subs[2].reset_week = st->zhipu_reset_week;
    st->subs[2].win_5h_s  = 18000;
    st->subs[2].week_s    = 604800;
    snprintf(st->subs[2].name, sizeof(st->subs[2].name), "Kimi Coding Plan");
    snprintf(st->subs[2].plan, sizeof(st->subs[2].plan), "Kimi Coding Pro");
    st->subs[2].valid = true;

    return true;
}

/* mock 预热：不发任何网络请求，仅模拟额度变化 + 前后说明。 */
static ds_prewarm_result_t mock_prewarm(gadget_state_t *st, char *msg, int msg_len)
{
    int before_5h = st->zhipu_used_5h;
    mock_ds_refresh(st);
    snprintf(msg, (size_t)msg_len, "预热完成（mock）：%d%% → %d%%",
             before_5h, st->zhipu_used_5h);
    return DS_PREWARM_OK;
}

/* mock 聊天：本地逐字"流式" echo，无网络。 */
static ds_chat_result_t mock_chat_stream(const ds_chat_msg_t *msgs, int n,
                                         ds_chat_delta_cb_t on_delta, void *user,
                                         char *reply_full, int reply_len)
{
    const char *last_user = "(空)";
    for (int i = n - 1; i >= 0; i--) {
        if (strcmp(msgs[i].role, "user") == 0) {
            last_user = msgs[i].content;
            break;
        }
    }
    snprintf(reply_full, (size_t)reply_len,
             "[mock] 你说：%s。这是模拟器 Mock 回复（聊天链路验证用）。",
             last_user);

    if (on_delta) {
        size_t len = strlen(reply_full);
        for (size_t i = 0; i < len; i++) {
            char one[8];
            /* 拷贝一个 UTF-8 字符（多字节连续输出，UI 不出现乱码） */
            size_t cl = 1 + ((reply_full[i] & 0xE0) == 0xC0 ? 1 : 0)
                          + ((reply_full[i] & 0xF0) == 0xE0 ? 2 : 0);
            if (i + cl > len) cl = len - i;
            memcpy(one, reply_full + i, cl);
            one[cl] = '\0';
            on_delta(one, user);
            i += cl - 1;
        }
    }
    return DS_CHAT_OK;
}

/* mock 语音交互：本地伪转写 + echo（无网络）。 */
static ds_voice_result_t mock_voice_interact(const char *app, const int16_t *pcm, int frames,
                                             char *transcript, int transcript_len,
                                             char *reply, int reply_len)
{
    (void)app;
    (void)pcm;
    (void)frames;
    static const char *utterances[] = {
        "现在几点了", "今天天气怎么样", "给我讲个笑话", "明天提醒我开会", "播放一首歌",
    };
    static int seq;
    const char *u = utterances[seq++ % 5];
    snprintf(transcript, (size_t)transcript_len, "%s", u);
    snprintf(reply, (size_t)reply_len,
             "[voice-mock] 收到「%s」。语音链路（ASR→LLM→TTS）已打通，这是模拟器本地回复。", u);
    return DS_VOICE_OK;
}

/* 天气：固定演示数据（深圳 晴） */
static int mock_fetch_weather(ds_weather_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    snprintf(out->location, sizeof(out->location), "深圳");
    snprintf(out->desc, sizeof(out->desc), "晴");
    snprintf(out->category, sizeof(out->category), "sun");
    out->temp_c = 26; out->feels_c = 27; out->humidity = 58; out->wind_kmh = 9;
    out->updated_unix = 1770000000;
    out->day_n = 5;
    const char *descs[] = {"晴", "多云", "小雨", "小雪", "雷阵雨"};
    for (int i = 0; i < 5; i++) {
        snprintf(out->days[i].date, sizeof(out->days[i].date), "2026-11-1%d", i);
        snprintf(out->days[i].desc, sizeof(out->days[i].desc), "%s", descs[i]);
        out->days[i].tmax = 24 + i;
        out->days[i].tmin = 15 + i;
    }
    return 0;
}

const ds_impl_t *ds_mock_impl(void)
{
    static const ds_impl_t impl = {
        .name            = "mock",
        .init            = mock_init,
        .refresh         = mock_ds_refresh,
        .prewarm_zhipu   = mock_prewarm,
        .chat_send_stream = mock_chat_stream,
        .voice_interact  = mock_voice_interact,
        .fetch_weather   = mock_fetch_weather,
        .device_config_fetch = mock_device_config_fetch,
    };
    return &impl;
}
