/* ui — 模拟器与固件共享的 UI 入口与页面导航。 */
#ifndef UI_H
#define UI_H

#include "lvgl.h"

/* App 目录（与 PLAN §5.1 对应；顺序即桌面网格顺序） */
enum ui_app_id {
    UI_APP_ZHIPU = 0,
    UI_APP_KIMI,
    UI_APP_MONITOR,
    UI_APP_ZCODE,
    UI_APP_CHAT,
    UI_APP_MUSE,
    UI_APP_XIAOZHI,
    UI_APP_MUSIC,
    UI_APP_SETTINGS,
    UI_APP_COUNT,
};

/* 构建全部页面并显示 Launcher（main 里调用一次） */
void ui_init(void);

/* 回到桌面 */
void ui_show_launcher(void);

/* 打开指定 App 页面 */
void ui_open_app(int app_id);

/* 各页面构建器（app_manager 注册用；screen 由 app_manager 预建） */
void ui_monitor_build(lv_obj_t *screen, int app_id);
void ui_conversation_build(lv_obj_t *screen, int app_id);
void ui_conversation_on_exit(void);
void ui_settings_build(lv_obj_t *screen, int app_id);

#endif /* UI_H */
