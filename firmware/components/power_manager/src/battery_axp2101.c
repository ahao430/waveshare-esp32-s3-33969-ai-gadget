/* battery_axp2101 — AXP2101 PMU 电池/充电状态读取。
 *
 * 寄存器语义（2026-11-14 定版，三源交叉验证 + 真机反推一致）：
 *   M5Unified AXP2101_Class.cpp / Zephyr charger_axp2101.c /
 *   OpenNextion xiaozhi axp2101.cc
 *   - 0x00 STATUS1: bit5=VBUS 在位, bit3=电池在位
 *   - 0x01 STATUS2: bit[6:5] 电流方向（1=充电 2=放电 0=待机）;
 *                   bit[2:0] 充电阶段（0-3=涓流/预充/恒流/恒压,
 *                   4=充电完成, 5=未充电）
 *   - 0x34/0x35: 电池电压 14bit（(H&0x3F)<<8|L，单位 mV）
 *   - 0xA4: 电量百分比 0-100
 * 真机首验：reg00=0x38（VBUS+电池在位）、reg01=0x32（恒流充电中）、
 * reg03=0x4A（ID）。
 */
#include "battery.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "axp2101";

#define AXP2101_I2C_ADDR 0x34

static i2c_master_bus_handle_t g_bus;
static bool g_present;

esp_err_t battery_init(void *i2c_bus)
{
    g_bus = (i2c_master_bus_handle_t)i2c_bus;
    if (g_bus == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = i2c_master_probe(g_bus, AXP2101_I2C_ADDR, 50);
    g_present = (err == ESP_OK);
    ESP_LOGI(TAG, "AXP2101 @0x%02X probe: %s", AXP2101_I2C_ADDR,
             g_present ? "ACK" : esp_err_to_name(err));
    return g_present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t read_regs(uint8_t reg, uint8_t *val, size_t len)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR, /* IDF v6：dev_addr → device_address */
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t handle;
    esp_err_t err = i2c_master_bus_add_device(g_bus, &dev, &handle);
    if (err != ESP_OK) return err;

    err = i2c_master_transmit_receive(handle, &reg, 1, val, len, -1);
    i2c_master_bus_rm_device(handle);
    return err;
}

esp_err_t battery_get_status(battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    if (!g_present || g_bus == NULL) return ESP_ERR_NOT_SUPPORTED;

    uint8_t s0 = 0, s1 = 0, vb[2] = { 0 }, pct = 0;
    if (read_regs(0x00, &s0, 1) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (read_regs(0x01, &s1, 1) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (read_regs(0x34, vb, 2) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (read_regs(0xA4, &pct, 1) != ESP_OK) return ESP_ERR_INVALID_STATE;

    status->battery_present = (s0 & 0x08) != 0;
    status->usb_connected = (s0 & 0x20) != 0;

    int dir = (s1 >> 5) & 0x3; /* 1=充电 2=放电 */
    int phase = s1 & 0x7;      /* 0-3 充电中，4 完成，5 未充 */
    status->charging = (dir == 1) || (phase <= 3);

    status->battery_voltage_mv = ((vb[0] & 0x3F) << 8) | vb[1];
    status->battery_percent = pct;

    ESP_LOGD(TAG, "s0=%02X s1=%02X vb=%dmV pct=%d%% usb=%d bat=%d chg=%d",
             s0, s1, status->battery_voltage_mv, pct,
             status->usb_connected, status->battery_present, status->charging);
    return ESP_OK;
}
