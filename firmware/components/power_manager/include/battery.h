/* battery — PMU 电池状态统一接口（用户拍板 2026-11-14，PLAN §13.3）。
 * 具体读数由 battery_axp2101（板载 PMU）实现；App 不直接访问 PMU，
 * 一律经 power_manager 轮询快照。 */
#ifndef POWER_MANAGER_BATTERY_H
#define POWER_MANAGER_BATTERY_H

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool battery_present;    /* 电池在位（false 时 UI 显示 USB 图标，不伪造 100%） */
    bool usb_connected;      /* USB 供电输入 */
    bool charging;           /* 充电中 */
    int battery_percent;     /* 0–100（fuel-gauge） */
    int battery_voltage_mv;  /* 电池电压 mV */
} battery_status_t;

/* 绝始化并探测 PMU（bus：BSP 共享 I²C 总线句柄；AXP2101 @ 0x34）。
 * 探测失败返回 ESP_ERR_NOT_FOUND，后续 get_status 返回 NOT_SUPPORTED。 */
esp_err_t battery_init(void *i2c_bus /* i2c_master_bus_handle_t，void* 避免头依赖 */);

/* 读取一次实时状态（阻塞 I²C 读，勿在 UI 任务里直接调用）。
 * TODO(bring-up)：寄存器语义按 AXP2101 数据手册核对后填充。 */
esp_err_t battery_get_status(battery_status_t *status);

#endif /* POWER_MANAGER_BATTERY_H */
