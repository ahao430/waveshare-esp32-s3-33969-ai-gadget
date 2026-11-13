#include "mock_data.h"

#include <stdlib.h>
#include <time.h>

static mock_state_t g_state = {
    .zhipu_used_5h    = 42,
    .zhipu_used_week  = 61,
    .zhipu_reset_5h   = 0, /* init 时填充 */
    .zhipu_reset_week = 0,
    .zhipu_plan       = "GLM Coding Plan (Lite)",

    .kimi_used_5h   = 12,
    .kimi_used_week = 35,

    .servers_online = 3,
    .servers_total  = 4,

    .battery_pct  = 82,
    .wifi_ok      = 1,
    .updated_unix = 0,
};

static int clamp_pct(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static void seed_state(void)
{
    time_t now = time(NULL);
    if (g_state.zhipu_reset_5h == 0) {
        g_state.zhipu_reset_5h   = (long)now + 3600 + rand() % 7200;
        g_state.zhipu_reset_week = (long)now + 86400 * 3;
        g_state.updated_unix     = (long)now;
    }
}

const mock_state_t *mock_state(void)
{
    seed_state();
    return &g_state;
}

void mock_refresh(void)
{
    seed_state();
    /* 模拟一次真实刷新：小幅随机波动 */
    g_state.zhipu_used_5h = clamp_pct(g_state.zhipu_used_5h + (rand() % 17) - 8);
    g_state.zhipu_used_week = clamp_pct(g_state.zhipu_used_week + (rand() % 5) - 2);
    g_state.kimi_used_5h = clamp_pct(g_state.kimi_used_5h + (rand() % 11) - 5);
    g_state.kimi_used_week = clamp_pct(g_state.kimi_used_week + (rand() % 7) - 3);
    g_state.updated_unix = (long)time(NULL);
}
