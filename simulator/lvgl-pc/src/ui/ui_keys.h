/* ui_keys — 物理键分发（POWER_MANAGEMENT §13.4 映射的软件侧）。
 * 三键语义（默认）：PWRON 短按=回首页 / BOOT 短按=返回 / KEY3 短按=当前
 * App 自定义（聊天=连续对话）/ KEY3 长按=语音 PTT（按住说话松手发出）。
 * 长按电源键（开关机，PMU 硬件）与 BOOT 长按（配网页）在固件侧，
 * 不可配置、模拟器不触发。
 * 映射可由 Web 后台配置（GET /api/v1/keys，ui_init 启动时拉取应用）。
 * 模拟器：GADGET_DEMO_KEYS="pw@2000,boot@4000,key3long@6000"（ms 时刻
 * 触发）headless 回归；GUI 阶段 F1/F2/F3 由 SDL 键盘 indev 映射。 */
#ifndef UI_KEYS_H
#define UI_KEYS_H

#include "lvgl.h"

/* 物理键 */
typedef enum {
    UI_KEY_PWR = 0,  /* 短按默认回首页 */
    UI_KEY_BOOT,     /* 短按默认返回 */
    UI_KEY_KEY3,     /* 短按默认当前 App 自定义；长按默认语音 PTT */
} ui_key_t;

/* 动作名（与 Bridge /api/v1/keys 值域一致） */
#define UI_KEY_ACT_HOME "home"
#define UI_KEY_ACT_BACK "back"
#define UI_KEY_ACT_NONE "none"
#define UI_KEY_ACT_APP  "app"
#define UI_KEY_ACT_PTT  "ptt"

/* 分发一次短按（input 服务/模拟器钩子调用） */
void ui_key_dispatch(ui_key_t key);

/* 分发一次长按（仅 KEY3 有可配置语义；PWR/BOOT 长按在固件侧） */
void ui_key_dispatch_long(ui_key_t key);

/* 长按释放收口（真机固件在 KEY3 松开时调用：按住说话、松开提交）。
 * 模拟器无释放事件源，GADGET_DEMO_KEYS 路径不经此（自动收尾）。 */
void ui_key_dispatch_long_release(ui_key_t key);

/* 应用 Web 下发的按键映射（NULL/未知值=保留默认；见 /api/v1/keys） */
void ui_keys_apply_config(const char *pwr_short, const char *boot_short,
                          const char *key3_short, const char *key3_long);

/* KEY3 短按的 App 自定义处理器（前台 App 注册；切 App 时更新） */
void ui_key_set_key3_handler(void (*cb)(void));

/* KEY3 长按的 App 处理器（映射=ptt 时触发；聊天=按住说话松手发出） */
void ui_key_set_key3_long_handler(void (*cb)(void));

/* KEY3 长按释放的 App 处理器（真机松开沿触发；映射=ptt 时收口录音） */
void ui_key_set_key3_long_release_handler(void (*cb)(void));

/* 模拟器：装载 GADGET_DEMO_KEYS 计划（ui_init 末尾调用；无钩子时无副作用） */
void ui_keys_demo_init(void);

#endif /* UI_KEYS_H */
