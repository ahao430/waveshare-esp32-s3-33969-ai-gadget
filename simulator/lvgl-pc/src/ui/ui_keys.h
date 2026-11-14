/* ui_keys — 物理键分发（POWER_MANAGEMENT §13.4 映射的软件侧）。
 * 三键语义：PWRON 短按=回首页 / BOOT 短按=返回 / KEY3 短按=当前 App
 * 自定义（聊天=连续对话开关）。长按在固件侧（input 服务 + PMU）。
 * 模拟器：GADGET_DEMO_KEYS="pw@2000,boot@4000"（ms 时刻触发）headless
 * 回归；GUI 阶段 F1/F2/F3 由 SDL 键盘 indev 映射（见 ui_keys.c）。 */
#ifndef UI_KEYS_H
#define UI_KEYS_H

#include "lvgl.h"

/* 物理键（短按） */
typedef enum {
    UI_KEY_PWR = 0,  /* 回首页 */
    UI_KEY_BOOT,     /* 返回 */
    UI_KEY_KEY3,     /* 当前 App 自定义 */
} ui_key_t;

/* 分发一次短按（input 服务/模拟器钩子调用） */
void ui_key_dispatch(ui_key_t key);

/* KEY3 短按的 App 自定义处理器（前台 App 注册；切 App 时更新） */
void ui_key_set_key3_handler(void (*cb)(void));

/* 模拟器：装载 GADGET_DEMO_KEYS 计划（ui_init 末尾调用；无钩子时无副作用） */
void ui_keys_demo_init(void);

#endif /* UI_KEYS_H */
