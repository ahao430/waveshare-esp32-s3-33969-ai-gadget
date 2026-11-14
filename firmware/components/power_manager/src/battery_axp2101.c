/* battery_axp2101 — AXP2101 PMU 读数骨架（真机 bring-up 前为探测 + TODO）。
 *
 * 硬件事实（docs/HARDWARE.md §2.6）：AXP2101 挂共享 I²C（SDA=15/SCL=14），
 * 7 位地址 0x34（社区默认，待 i2cdetect 确认）；电池为单节 3.7V 锂电池。
 *
 * bring-up 步骤（Phase 6，BRINGUP.md §逐驱动）：
 *   1. i2c_master_probe(0x34) → ACK 后进入下一步
 *   2. 按数据手册核对本文件 TODO 表中的寄存器地址与位定义
 *      （不同批次/固件的 AXP2101 电量计寄存器存在差异，勿凭记忆定版）
 *   3. 依次实现 read_reg/write_reg → 各 getter → 删除对应 TODO
 */
#include "battery.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "axp2101";

#define AXP2101_I2C_ADDR 0x34

/* ---- 寄存器表（地址以数据手册为准，全部待核对） ----
 * reg   用途                          状态
 * 0x00? PMU 状态（USB 在位/充电激活）  TODO 手册核对
 * 0x??  电量百分比（fuel gauge）       TODO
 * 0x??  电池电压（V sense，高位+低位） TODO
 * 0x??  电池在位检测位                 TODO
 */
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

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR, /* IDF v6：dev_addr → device_address */
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t handle;
    esp_err_t err = i2c_master_bus_add_device(g_bus, &dev, &handle);
    if (err != ESP_OK) return err;

    err = i2c_master_transmit_receive(handle, &reg, 1, val, 1, -1);
    i2c_master_bus_rm_device(handle);
    return err;
}

esp_err_t battery_get_status(battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    if (!g_present || g_bus == NULL) return ESP_ERR_NOT_SUPPORTED;

    /* TODO(bring-up)：按上方寄存器表实现（探测已通过即到此返回）。
     * 现返回 NOT_SUPPORTED，power_manager 会保留上一次有效快照并
     * 以日志提示，避免 UI 显示编造数值。 */
    uint8_t v = 0;
    esp_err_t err = read_reg(0x00, &v); /* 探测性读：仅验证链路 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read_reg: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "link ok (reg00=0x%02X); datasheet TODO pending", v);
    return ESP_ERR_NOT_SUPPORTED;
}
