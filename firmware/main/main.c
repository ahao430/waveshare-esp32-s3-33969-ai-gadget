/* AI Gadget 固件骨架（Phase 0/6 准备）。
 * 上电流程：
 *   1. I2C 总线（BSP）→ 全地址扫描并打印外设识别表
 *      （对照 docs/HARDWARE.md §2.6 的待验证地址清单——真机首验的关键证据）
 *   2. BSP 显示 → 点亮 + 版本文字
 *
 * TODO（未烧录验证，设备到手按 docs/BRINGUP.md 执行）：
 *  - i2c_master_probe 的超时/错误码在真机上的实际表现
 *  - PSRAM 显示缓冲策略（Muse 移植报告 PSRAM 绘制在 Wi-Fi/BLE 后失败，
 *    见 docs/HARDWARE.md §2.2）
 */
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "lvgl.h"
#include "power_manager.h"

static const char *TAG = "gadget";

/* 共享 I2C 总线（SDA=15, SCL=14）上预期存在的芯片。
 * 地址为 docs/HARDWARE.md §2.6 的社区默认值，以扫描结果为准。 */
static const struct {
    uint8_t addr;
    const char *chip;
} g_expected[] = {
    { 0x34, "AXP2101 PMU" },
    { 0x51, "PCF85063 RTC" },
    { 0x6B, "QMI8658 IMU" },
    { 0x18, "ES8311 codec" },
    { 0x40, "ES7210 ADC" },
    { 0x5A, "CST9220 touch" },
};

static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle(); /* BSP：返回值而非出参 */
    if (bus == NULL) {
        ESP_LOGE(TAG, "i2c bus not available (bsp_i2c_get_handle)");
        return;
    }

    ESP_LOGI(TAG, "=== I2C scan (0x08-0x77) ===");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(bus, addr, 50);
        if (err == ESP_OK) {
            const char *known = "";
            for (size_t i = 0; i < sizeof(g_expected) / sizeof(g_expected[0]); i++) {
                if (g_expected[i].addr == addr) {
                    known = g_expected[i].chip;
                    break;
                }
            }
            ESP_LOGI(TAG, "  0x%02X ACK  %s", addr, known);
        }
    }
    ESP_LOGI(TAG, "=== scan done（对照 docs/HARDWARE.md §2.6）===");
}

void app_main(void)
{
    i2c_scan();

    /* 电源管理（PLAN §13.3）：AXP2101 轮询 30s/15s + 低电策略。
     * PMU 探测失败只告警不阻塞启动（UI 显示 USB/-- 兜底）。 */
    esp_err_t pm_err = power_manager_init(bsp_i2c_get_handle());
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "power_manager unavailable: %s（UI 显示 USB/-- 兜底）",
                 esp_err_to_name(pm_err));
    }

    /* BSP: CO5300 QSPI 显示 + LVGL 任务（缓冲策略见 sdkconfig.defaults） */
    bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(0);
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B1020), 0);

    lv_obj_t *label = lv_label_create(root);
    lv_label_set_text(label, "AI Gadget\nbringup build - Phase 6 prep");
    lv_obj_set_style_text_color(label, lv_color_hex(0xEAF0FF), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    bsp_display_unlock();
}
