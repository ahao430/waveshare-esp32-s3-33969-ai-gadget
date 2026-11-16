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

    .sub_count = 0, /* subs 未列出 = 零填充（designated init 语义，
                       显式 {{0}} 反触发新 GCC missing-braces 告警） */

    .battery_pct  = 82,
    .wifi_ok      = true,
    .updated_unix = 0,
    .battery_present    = true,
    .usb_connected      = true,
    .charging           = true,
    .battery_voltage_mv = 4120,

    .brightness        = 80,
    .volume            = 55,
    .display_timeout_s = 60,
    .sub_refresh_interval_s = 900,
};

/* GADGET_DEMO_BATTERY="pct,mv,state"（state: idle|charging|usb|none）
 * —— 状态栏/设备信息页的电源场景演示钩子；
 * GADGET_DEMO_THEME=n —— 主题演示钩子（ui_theme k_ui_themes 下标） */
static void demo_battery_apply(void)
{
    const char *th = getenv("GADGET_DEMO_THEME");
    if (th && th[0]) {
        int idx = atoi(th);
        if (idx >= 0 && idx < 4) g_state.theme_idx = idx;
    }

    const char *v = getenv("GADGET_DEMO_BATTERY");
    if (!v || !v[0]) return;

    int pct = g_state.battery_pct;
    int mv = g_state.battery_voltage_mv;
    char state[12] = "";
    if (sscanf(v, "%d,%d,%11s", &pct, &mv, state) < 1) return;

    if (pct >= 0 && pct <= 100) g_state.battery_pct = pct;
    if (mv > 2500 && mv < 5000) g_state.battery_voltage_mv = mv;
    if (strcmp(state, "charging") == 0) {
        g_state.battery_present = true;
        g_state.usb_connected = true;
        g_state.charging = true;
    } else if (strcmp(state, "usb") == 0) {
        g_state.battery_present = false; /* USB 供电、未检到电池 */
        g_state.usb_connected = true;
        g_state.charging = false;
    } else if (strcmp(state, "none") == 0) {
        g_state.battery_present = false;
        g_state.usb_connected = false;
        g_state.charging = false;
    } else { /* idle：电池供电不充电 */
        g_state.battery_present = true;
        g_state.usb_connected = false;
        g_state.charging = false;
    }
}

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

void ds_config_set_theme(int idx)
{
    g_state.theme_idx = idx;
}

static const ds_impl_t *g_impl;
static char g_chat_provider[32] = "mock";

int ds_chat_reset(const char *bot_id)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->chat_reset) return 0; /* mock/不支持：视为已清（本地已清） */
    return g_impl->chat_reset(bot_id ? bot_id : "");
}

void ds_init(const char *source, const char *bridge_url, const char *provider)
{
    /* 已初始化且本次没给参数 = 不要动（只补默认值语义）。
     * 真机事故（2026-11-15）：main 先 ds_init("bridge", BRIDGE_HOST)，
     * 随后 ui_init() 为了"从环境变量初始化"又调 ds_init(NULL,NULL,NULL)——
     * 设备上没有 GADGET_DATA_SOURCE，直接回落 mock，把 bridge 实现覆盖掉。
     * 后果：语音走 mock 假数据（4ms 返回固定文本，ASR/LLM/TTS 全不执行）、
     * 天气永远深圳、Web 下发配置不生效。 */
    if (!source && !bridge_url && !provider && g_impl) return;

    /* 聊天 provider 默认值：GADGET_CHAT_PROVIDER（bridge 模式有意义） */
    if (getenv("GADGET_CHAT_PROVIDER") && getenv("GADGET_CHAT_PROVIDER")[0]) {
        snprintf(g_chat_provider, sizeof(g_chat_provider), "%s",
                 getenv("GADGET_CHAT_PROVIDER"));
    }
    const char *src  = source  ? source  : getenv("GADGET_DATA_SOURCE");
    const char *url  = bridge_url ? bridge_url : getenv("GADGET_BRIDGE_URL");
    const char *prov = provider ? provider : getenv("GADGET_BRIDGE_PROVIDER");

    bool is_bridge = false;
    if (src && strcmp(src, "bridge") == 0) {
        is_bridge = true;
        g_impl = ds_bridge_impl(url ? url : "http://127.0.0.1:8081",
                                prov ? prov : "mock");
    } else {
        g_impl = ds_mock_impl();
    }
    if (g_impl->init) g_impl->init();
    /* mock（本地数据）init 后立即填一次缓存——ds_state 不再惰性刷新，
     * 模拟器首屏要有演示数据。bridge 模式不在此拉网络（真机此时 Wi-Fi
     * 还没起；数据由各页 worker 显式刷新）。 */
    if (!is_bridge && g_impl->refresh) {
        (void)g_impl->refresh(&g_state);
    }
    demo_battery_apply();
}

const gadget_state_t *ds_state(void)
{
    /* 只读缓存——绝不在调用方上下文里发起网络（2026-11-16 栈扫描定位）：
     * 旧实现的"首次访问惰性刷新"在 Bridge 不可达时 updated_unix 恒为 0，
     * 状态栏（5s）/倒计时（1s）等 LVGL 回调每次 ds_state 都同步发 HTTP
     * （connect 5s 超时）→ LVGL 任务持显示锁冻屏。首屏数据由各页 worker
     * 显式 ds_refresh；mock 在 ds_init 时预填。 */
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

int ds_chat_bots(char *out, int cap)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (out && cap > 0) out[0] = '\0';
    if (!g_impl->chat_bots) return 1;
    return g_impl->chat_bots(out, cap);
}

int ds_chat_active_id(char *out, int cap)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (out && cap > 0) out[0] = '\0';
    if (!g_impl->chat_active_id) return 1;
    return g_impl->chat_active_id(out, cap);
}

int ds_chat_set_active(const char *id)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->chat_set_active) return 1;
    return g_impl->chat_set_active(id);
}

ds_voice_result_t ds_voice_interact(const char *app, const int16_t *pcm, int frames,
                                    char *transcript, int transcript_len,
                                    char *reply, int reply_len)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->voice_interact) {
        snprintf(reply, (size_t)reply_len, "当前数据源不支持语音");
        return DS_VOICE_UNSUPPORTED;
    }
    return g_impl->voice_interact(app, pcm, frames, transcript, transcript_len, reply, reply_len);
}

int ds_appearance_fetch(const char *save_path)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->fetch_background) return 1;
    return g_impl->fetch_background(save_path);
}

int ds_appearance_fetch_theme(int *theme)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->fetch_theme) return 1;
    return g_impl->fetch_theme(theme);
}

int ds_launcher_fetch(char *out_csv, int cap, int *count)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->fetch_launcher) return 1;
    return g_impl->fetch_launcher(out_csv, cap, count);
}

int ds_weather_fetch(ds_weather_t *out)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->fetch_weather) {
        if (out) out->valid = false;
        return 1;
    }
    return g_impl->fetch_weather(out);
}

int ds_keys_fetch(char *pwr_short, char *boot_short,
                  char *key3_short, char *key3_long, int cap)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->fetch_keys) return 1;
    return g_impl->fetch_keys(pwr_short, boot_short, key3_short, key3_long, cap);
}

int ds_device_config_fetch(int *sub_refresh_interval_s)
{
    if (!g_impl) ds_init(NULL, NULL, NULL);
    if (!g_impl->device_config_fetch) return -1;
    return g_impl->device_config_fetch(sub_refresh_interval_s);
}
