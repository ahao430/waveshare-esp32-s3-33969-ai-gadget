#include "data_source_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* data_source_mock.c / data_source_bridge.c 提供 */
const ds_impl_t *ds_mock_impl(void);
const ds_impl_t *ds_bridge_impl(const char *bridge_url, const char *provider);

static gadget_state_t g_state = {
    .zhipu_used_5h    = 42,
    .zhipu_used_week  = 61,
    .zhipu_reset_5h   = 0, /* 各实现 init/refresh 时填充 */
    .zhipu_reset_week = 0,
    .zhipu_plan       = "GLM Coding Plan",

    .kimi_used_5h   = 12,
    .kimi_used_week = 35,

    .sub_count = 2,
    .subs = {
        { "智谱", "GLM Coding Plan (Lite)", 42, 61, 18000, 604800, 0, 0, true },
        { "Kimi", "Kimi Coding Plan",        12, 35, 18000, 604800, 0, 0, true },
    },

    .server_count = 4,
    .servers = {
        { "hk-01", true,  23, 55, 38,  540, 1560, 120 },
        { "hk-02", true,  48, 66, 41,  890, 2450, 730 },
        { "jp-01", false,  0,  0,  0,    0,    0,   0 },
        { "us-01", true,  71, 82, 77, 1320, 3980, 240 },
    },

    .battery_pct  = 82,
    .wifi_ok      = true,
    .updated_unix = 0,

    .brightness        = 80,
    .volume            = 55,
    .display_timeout_s = 60,
    .sub_refresh_interval_s = 900,
};

void ds_config_update(int brightness, int volume, int timeout_s)
{
    if (brightness > 0) g_state.brightness = brightness;
    if (volume >= 0) g_state.volume = volume;
    if (timeout_s > 0) g_state.display_timeout_s = timeout_s;
    /* TODO(bridge 模式)：同步 PUT 设备 config API（Phase 6 设备侧就绪后） */
}

void ds_config_set_refresh_interval(int seconds)
{
    if (seconds > 0) g_state.sub_refresh_interval_s = seconds;
}

int ds_config_refresh_interval(void)
{
    return g_state.sub_refresh_interval_s;
}

static const ds_impl_t *g_impl;
static char g_chat_provider[32] = "mock";

void ds_init(const char *source, const char *bridge_url, const char *provider)
{
    /* 聊天 provider 默认值：GADGET_CHAT_PROVIDER（bridge 模式有意义） */
    if (getenv("GADGET_CHAT_PROVIDER") && getenv("GADGET_CHAT_PROVIDER")[0]) {
        snprintf(g_chat_provider, sizeof(g_chat_provider), "%s",
                 getenv("GADGET_CHAT_PROVIDER"));
    }
    const char *src  = source  ? source  : getenv("GADGET_DATA_SOURCE");
    const char *url  = bridge_url ? bridge_url : getenv("GADGET_BRIDGE_URL");
    const char *prov = provider ? provider : getenv("GADGET_BRIDGE_PROVIDER");

    if (src && strcmp(src, "bridge") == 0) {
        g_impl = ds_bridge_impl(url ? url : "http://127.0.0.1:8081",
                                prov ? prov : "mock");
    } else {
        g_impl = ds_mock_impl();
    }
    if (g_impl->init) g_impl->init();
}

const gadget_state_t *ds_state(void)
{
    if (g_state.updated_unix == 0) ds_refresh(); /* 首次访问惰性刷新 */
    return &g_state;
}

bool ds_refresh(void)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    return g_impl->refresh(&g_state);
}

const char *ds_source_name(void)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    return g_impl->name;
}

ds_prewarm_result_t ds_prewarm_zhipu(char *msg, int msg_len)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->prewarm_zhipu) {
        snprintf(msg, (size_t)msg_len, "当前数据源不支持预热");
        return DS_PREWARM_UNSUPPORTED;
    }
    return g_impl->prewarm_zhipu(&g_state, msg, msg_len);
}

ds_chat_result_t ds_chat_send_stream(const ds_chat_msg_t *msgs, int n,
                                     ds_chat_delta_cb_t on_delta, void *user,
                                     char *reply_full, int reply_len)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->chat_send_stream) {
        snprintf(reply_full, (size_t)reply_len, "当前数据源不支持聊天");
        return DS_CHAT_UNSUPPORTED;
    }
    return g_impl->chat_send_stream(msgs, n, on_delta, user, reply_full, reply_len);
}

int ds_chat_providers(char *out, int cap)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->chat_providers) {
        snprintf(out, (size_t)cap, "mock");
        return 0;
    }
    return g_impl->chat_providers(out, cap);
}

void ds_chat_set_provider(const char *id)
{
    snprintf(g_chat_provider, sizeof(g_chat_provider), "%s", id ? id : "mock");
}

const char *ds_chat_provider(void)
{
    return g_chat_provider;
}

ds_voice_result_t ds_voice_interact(const int16_t *pcm, int frames,
                                    char *transcript, int transcript_len,
                                    char *reply, int reply_len)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->voice_interact) {
        snprintf(reply, (size_t)reply_len, "当前数据源不支持语音");
        return DS_VOICE_UNSUPPORTED;
    }
    return g_impl->voice_interact(pcm, frames, transcript, transcript_len, reply, reply_len);
}
