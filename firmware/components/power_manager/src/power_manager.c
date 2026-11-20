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
#define POLL_LOWBATT_MS (5 * 1000)  /* 电池供电且 <3.55V：加密观测（2026-11-20） */

/* 电压耗尽保护（2026-11-20）：AXP2101 无库仑计，0xA4 电量在电池
 * 老化/重载下偏高，用户遇到"显示 80%+ 被 PMU 欠压硬断电、开机即断"。
 * PMU 硬断电不给我们任何机会（文件系统/进度裸断）——在它断电之前
 * 按电压提前优雅关机：3.35V 持续 2 次读数（5s 档 ≈10s）即软件关机。 */
#define PM_EXHAUST_VMV 3350
#define PM_EXHAUST_N   2
#define PM_WARN_VMV    3450

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
    static bool last_charging = false, last_usb = false, last_present = false;
    static int exhaust_cnt;

    bool changed = (st->battery_percent != last_pct) ||
                   (st->charging != last_charging) ||
                   (st->usb_connected != last_usb) ||
                   (st->battery_present != last_present);
    if (changed) {
        last_pct = st->battery_percent;
        last_charging = st->charging;
        last_usb = st->usb_connected;
        last_present = st->battery_present;
    }

    bool on_batt = st->battery_present && !st->usb_connected && !st->charging;

    /* 电压耗尽：提前于 PMU 硬断电优雅关机（文件系统/进度不裸断） */
    if (on_batt && st->battery_voltage_mv > 2500 &&
        st->battery_voltage_mv <= PM_EXHAUST_VMV) {
        if (++exhaust_cnt >= PM_EXHAUST_N) {
            ESP_LOGW(TAG, "battery exhausted (%dmV %d%%) - graceful power off",
                     st->battery_voltage_mv, st->battery_percent);
            (void)battery_power_off();
            return;
        }
        ESP_LOGW(TAG, "battery near exhaustion (%dmV %d%%)",
                 st->battery_voltage_mv, st->battery_percent);
    } else {
        exhaust_cnt = 0;
        if (on_batt && st->battery_voltage_mv <= PM_WARN_VMV) {
            ESP_LOGW(TAG, "battery low voltage (%dmV %d%%) - charge soon",
                     st->battery_voltage_mv, st->battery_percent);
        }
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
        /* 每轮一行观测（30/15s 一条；低电压档 5s 一条）：电压/电量
         * 关系是校准钳制曲线的依据，真机排障靠它（2026-11-20） */
        ESP_LOGI(TAG, "battery: %dmV %d%% usb=%d chg=%d",
                 st.battery_voltage_mv, st.battery_percent,
                 st.usb_connected, st.charging);
        apply_policy(&st);
    } else {
        ESP_LOGD(TAG, "battery read failed; keep last snapshot");
    }

    /* 轮询频率：充电 15s；电池供电且 <3.55V 5s（耗尽确认要 10s 内完成）；
     * 其余 30s。esp_timer 周期单位是 µs。 */
    uint64_t period_ms = POLL_NORMAL_MS;
    if (g_last.charging) {
        period_ms = POLL_CHARGING_MS;
    } else if (g_last.battery_present && !g_last.usb_connected &&
               g_last.battery_voltage_mv > 2500 &&
               g_last.battery_voltage_mv <= 3550) {
        period_ms = POLL_LOWBATT_MS;
    }
    esp_timer_stop(g_timer);
    esp_timer_start_periodic(g_timer, period_ms * 1000ULL);
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
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_timer, POLL_NORMAL_MS * 1000ULL));
    poll_timer_cb(NULL); /* 启动即读一次 */
    g_inited = true;
    return ESP_OK;
}

/* 抑制未用告警（g_inited 留作后续 is_running 接口） */
__attribute__((unused)) static bool pm_inited(void) { return g_inited; }

esp_err_t power_manager_keys_init(void) { return battery_keys_init(); }
unsigned power_manager_poll_key(void) { return battery_poll_key(); }
esp_err_t power_manager_power_off(void) { return battery_power_off(); }
