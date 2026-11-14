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

    /* 订阅列表与旧字段同步 */
    if (st->sub_count >= 1) {
        st->subs[0].used_5h   = st->zhipu_used_5h;
        st->subs[0].used_week = st->zhipu_used_week;
        st->subs[0].reset_5h  = st->zhipu_reset_5h;
        st->subs[0].reset_week = st->zhipu_reset_week;
        st->subs[0].valid = true;
    }
    if (st->sub_count >= 2) {
        st->subs[1].used_5h   = st->kimi_used_5h;
        st->subs[1].used_week = st->kimi_used_week;
        st->subs[1].valid = true;
    }

    /* 服务器队列伪随机波动 */
    for (int i = 0; i < st->server_count && i < DS_SERVER_MAX; i++) {
        gadget_server_t *s = &st->servers[i];
        if (!s->online) {
            if (rand() % 10 == 0) { /* 偶尔复活 */
                s->online = true;
                s->cpu_pct = 10 + rand() % 30;
                s->mem_pct = 30 + rand() % 40;
            }
            continue;
        }
        s->cpu_pct += rand() % 15 - 7;
        s->mem_pct += rand() % 9 - 4;
        if (s->cpu_pct < 1) s->cpu_pct = 1;
        if (s->cpu_pct > 99) s->cpu_pct = 99;
        if (s->mem_pct < 5) s->mem_pct = 5;
        if (s->mem_pct > 99) s->mem_pct = 99;
        if (rand() % 25 == 0) { /* 偶尔掉线 */
            s->online = false;
            s->cpu_pct = s->mem_pct = 0;
        }
    }
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
static ds_voice_result_t mock_voice_interact(const int16_t *pcm, int frames,
                                             char *transcript, int transcript_len,
                                             char *reply, int reply_len)
{
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

const ds_impl_t *ds_mock_impl(void)
{
    static const ds_impl_t impl = {
        .name            = "mock",
        .init            = mock_init,
        .refresh         = mock_ds_refresh,
        .prewarm_zhipu   = mock_prewarm,
        .chat_send_stream = mock_chat_stream,
        .voice_interact  = mock_voice_interact,
    };
    return &impl;
}
