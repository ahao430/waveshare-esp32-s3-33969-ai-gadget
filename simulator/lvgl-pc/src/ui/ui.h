/* ui — 模拟器与固件共享的 UI 入口与页面导航。 */
#ifndef UI_H
#define UI_H

#include "lvgl.h"

/* 显示锁抽象：非 LVGL 线程（音频 worker/播放任务）触碰 LVGL 对象时
 * 必须持有（真机坑#1：bsp_display_lock(0)=无限等待的递归互斥量，
 * 有限超时 take 会触发 FreeRTOS 断言——只许 lock(0)+成对 unlock）。
 * 模拟器单线程 no-op。 */
#ifdef ESP_PLATFORM
void ui_lock(void);
void ui_unlock(void);
#else
static inline void ui_lock(void) {}
static inline void ui_unlock(void) {}
#endif

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
    UI_APP_WEATHER, /* 天气（ADR-005 清单，Phase 5+） */
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
void ui_weather_build(lv_obj_t *screen, int app_id); /* 天气（Open-Meteo） */
void ui_weather_on_exit(void);
void ui_conversation_build(lv_obj_t *screen, int app_id);
void ui_conversation_on_exit(void);
void ui_settings_build(lv_obj_t *screen, int app_id);
void ui_settings_on_exit(void);
void ui_settings_build_page(lv_obj_t *screen); /* 分类子页重建（nav 用） */

#endif /* UI_H */
