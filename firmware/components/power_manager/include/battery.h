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

/* ---- PWR 键（AXP2101 PWRON；2026-11-16）----
 * PWRON 不是 GPIO（IRQ 引脚未接 ESP32，HARDWARE §2.9），按住/松开
 * 事件经 PMU 中断状态寄存器轮询获取。 */
#define BATT_KEY_PRESS   (1u << 0) /* 按下沿 */
#define BATT_KEY_RELEASE (1u << 1) /* 松开沿 */
#define BATT_KEY_CLICK   (1u << 2) /* 短按（PMU 判定） */
#define BATT_KEY_LONG    (1u << 3) /* 长按（PMU 判定） */

/* 配置 PWR 键：长按 10s 硬关兜底 + 使能长按过关断即关 + 使能键事件
 * 状态位（供 battery_poll_key 轮询）。battery_init 成功后调用。 */
esp_err_t battery_keys_init(void);

/* 轮询键事件（读 INTSTS2，写 1 清零；无事件返回 0）。~100ms 一次。 */
unsigned battery_poll_key(void);

/* 软件关机（REG10 bit0；读改写，绝不动 bit1 复位命令位）。 */
esp_err_t battery_power_off(void);

#endif /* POWER_MANAGER_BATTERY_H */
