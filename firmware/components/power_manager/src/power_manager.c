/* power_manager — 轮询与低电策略实现，接口见 power_manager.h。 */
#include "power_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pwr";

#define POLL_NORMAL_MS  (30 * 1000) /* 正常 30s（用户拍板） */
#define POLL_CHARGING_MS (15 * 1000) /* 充电中 15s */

static battery_status_t g_last;
static bool g_inited;
static esp_timer_handle_t g_timer;
static power_manager_cb_t g_cb;
static void *g_cb_user;

battery_status_t power_manager_snapshot(void)
{
    /* 读快照与轮询任务并发：结构体小（5 字段），暂用临界区保护 */
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    battery_status_t copy;
    taskENTER_CRITICAL(&mux);
    copy = g_last;
    taskEXIT_CRITICAL(&mux);
    return copy;
}

void power_manager_subscribe(power_manager_cb_t cb, void *user)
{
    g_cb = cb;
    g_cb_user = user;
}

/* 状态突变判断 + 低电策略 + 回调分发 */
static void apply_policy(const battery_status_t *st)
{
    static int last_pct = -1;
    static bool last_charging = false;

    bool changed = (st->battery_percent != last_pct) ||
                   (st->charging != last_charging);
    if (changed) {
        last_pct = st->battery_percent;
        last_charging = st->charging;
    }

    if (st->battery_present && st->battery_percent < PM_CRITICAL_BATTERY_PCT) {
        /* TODO(Phase 6)：进入节能策略——降亮度、缩短息屏、暂停高耗任务；
         * 策略允许用户在设置中关闭/调整（PRODUCT_SPEC §4） */
        ESP_LOGW(TAG, "critical battery (%d%%) - power save policy", st->battery_percent);
    } else if (st->battery_present && st->battery_percent < PM_LOW_BATTERY_PCT) {
        /* TODO(Phase 6)：系统通知"请充电"（事件总线 → UI toast） */
        ESP_LOGW(TAG, "low battery (%d%%) - charge reminder", st->battery_percent);
    }

    if (changed && g_cb) g_cb(st, g_cb_user);
}

static void poll_timer_cb(void *arg)
{
    (void)arg;
    battery_status_t st;
    if (battery_get_status(&st) == ESP_OK) {
        g_last = st;
        apply_policy(&st);
    } else {
        ESP_LOGD(TAG, "battery read failed; keep last snapshot");
    }

    /* 充电态提高轮询频率（15s），否则 30s */
    uint64_t period = g_last.charging ? POLL_CHARGING_MS : POLL_NORMAL_MS;
    esp_timer_stop(g_timer);
    esp_timer_start_periodic(g_timer, period);
}

esp_err_t power_manager_init(void *i2c_bus)
{
    esp_err_t err = battery_init(i2c_bus);
    if (err != ESP_OK) {
        /* PMU 不在位：保持"未知"快照（UI 显示 USB/--），不阻塞启动 */
        ESP_LOGE(TAG, "battery_init: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t args = {
        .callback = poll_timer_cb,
        .name = "pm_poll",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &g_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_timer, POLL_NORMAL_MS));
    poll_timer_cb(NULL); /* 启动即读一次 */
    g_inited = true;
    return ESP_OK;
}

/* 抑制未用告警（g_inited 留作后续 is_running 接口） */
__attribute__((unused)) static bool pm_inited(void) { return g_inited; }
