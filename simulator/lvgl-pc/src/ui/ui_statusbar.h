/* ui_statusbar — 全局状态栏 overlay（PLAN §13.3 电池与充电）。
 * 挂在 lv_layer_top 上常驻：所有屏幕（Launcher/App）顶部统一显示
 * Wi-Fi + 电量；点击穿透；模态（后建）会盖住它（符合预期）。
 *
 * 显示规则（用户拍板，阈值可后续配置化）：
 *   电池五档图标：≥80 FULL / ≥60 _3 / ≥40 _2 / ≥20 _1 / <20 EMPTY
 *   颜色：≥20 正常 / 10–19 低电（WARN）/ 0–9 紧急（ERR）
 *   充电：图标 + 绿色闪电（LV_SYMBOL_CHARGE）
 *   USB 供电未检到电池：USB 图标，不显示虚假的 100%
 *
 * 数据来自 data_source（mock=演示值；固件=AXP2101 经 Power Manager 回填）。
 */
#ifndef UI_STATUSBAR_H
#define UI_STATUSBAR_H

#include "lvgl.h"

/* 创建 overlay 并启动 5s 刷新定时器（ui_init 调一次；幂等） */
void ui_statusbar_init(void);

/* 打开快速音量弹窗（对话页音量按钮复用；状态栏图标簇点击同款） */
void ui_statusbar_volume_popup(void);

/* 把 ds_state() 的音量/亮度应用到硬件（codec + 面板 DCS）。开机补套
 * 与远程配置应用后各调一次；模拟器 no-op。 */
void ui_statusbar_apply_hw(void);

/* 立即按 ds_state() 刷新（低电量事件等场景可主动调） */
void ui_statusbar_refresh(void);

/* App 后台状态槽（P1）：番茄钟后台计时等在状态栏顶部中央显示小药丸。
 * text=NULL/空 隐藏；color 为文字色（通常传阶段色）。仅在
 * ui_statusbar_init() 之后生效；不新增任务/线程，由调用方既有 lv_timer
 * 驱动（番茄钟 tick 已有 1s 节流）。 */
void ui_statusbar_set_app_status(const char *text, uint32_t color);

#endif /* UI_STATUSBAR_H */
