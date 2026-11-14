/* power_manager — 电源管理服务（PLAN §13.3）。
 * 职责（App 不得绕过本组件直接访问 PMU）：
 *   - 定期轮询电池状态：正常 30s / 充电中 15s / 状态突变立即回调
 *   - 低电策略：<15% 提醒充电；<5% 进入节能（降亮度/缩短息屏）
 *   - 快照供 UI（状态栏/设备信息页）读取
 */
#ifndef POWER_MANAGER_POWER_MANAGER_H
#define POWER_MANAGER_POWER_MANAGER_H

#include <stdbool.h>
#include "battery.h"
#include "esp_err.h"

/* 低电策略阈值（用户拍板 2026-11-14；UI 同步显示规则见
 * simulator ui_statusbar.c——20%/10% 档为显示色，本处为策略档） */
#define PM_LOW_BATTERY_PCT    15 /* 低于此值：提醒充电 */
#define PM_CRITICAL_BATTERY_PCT 5 /* 低于此值：节能策略 */

typedef void (*power_manager_cb_t)(const battery_status_t *st, void *user);

/* 初始化 PMU 并启动轮询定时器。i2c_bus = bsp_i2c_get_handle()。 */
esp_err_t power_manager_init(void *i2c_bus);

/* 最近一次快照（中断/多任务安全读；未初始化时 present=false） */
battery_status_t power_manager_snapshot(void);

/* 订阅状态变化（状态突变或越阈时回调；在定时器任务上下文，勿阻塞） */
void power_manager_subscribe(power_manager_cb_t cb, void *user);

#endif /* POWER_MANAGER_POWER_MANAGER_H */
