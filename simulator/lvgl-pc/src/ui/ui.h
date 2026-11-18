/* ui — 模拟器与固件共享的 UI 入口与页面导航。 */
#ifndef UI_H
#define UI_H

#include "lvgl.h"

/* 显示锁抽象：非 LVGL 线程（音频 worker/播放任务）触碰 LVGL 对象时
 * 必须持有。实现用 esp_lv_adapter_lock(-1)（无限等待的递归互斥量）。
 * 注意：bsp_display_lock(0) 在本版 BSP/adapter 里是 0-tick 非阻塞
 * try-lock（头文件写"0=无限等待"，实现不符），失败被忽略就会并发踩
 * LVGL → 命中 LV_ASSERT_MSG("Invalidate area is not allowed during
 * rendering") → LV_ASSERT_HANDLER 默认 while(1) 静默死机（真机实测：
 * 屏幕半帧花屏 + IDLE0 task_wdt）。只许 -1 + 成对 unlock。
 * 模拟器单线程 no-op。 */
#ifdef ESP_PLATFORM
void ui_lock(void);
void ui_unlock(void);
#else
static inline void ui_lock(void) {}
static inline void ui_unlock(void) {}
#endif

/* App 目录（与 PLAN §5.1 对应；顺序即桌面网格顺序）。
 * 2026-11-16 精简：删 ZCode 远程/服务器监控（全链路下线，见 ADR-010），
 * 同步清理了无引用的 KIMI 枚举——枚举值有变化，GADGET_DEMO_APP 数字
 * 钩子按新枚举取值。 */
enum ui_app_id {
    UI_APP_ZHIPU = 0,
    UI_APP_CHAT,
    UI_APP_MUSE,
    UI_APP_XIAOZHI,
    UI_APP_MUSIC,
    UI_APP_FILES, /* 文件管理器（TF 卡，ADR-010） */
    UI_APP_SETTINGS,
    UI_APP_WEATHER, /* 天气（ADR-005 清单，Phase 5+） */
    UI_APP_POMODORO, /* 番茄钟（25/5 循环，2026-11-16） */
    UI_APP_READER, /* 小说阅读器（ADR-017，TXT） */
    UI_APP_PHOTO,  /* 图片（ADR-018，JPEG/PNG/BMP） */
    UI_APP_COUNT,
};

/* 构建全部页面并显示 Launcher（main 里调用一次） */
/* 低频后台 worker 投递到共享静态任务（真机；16KB 栈，零新增 .bss）。
 * 返回 0=已排队 -1=任务创建失败 -2=忙（语音回合/其它 worker 执行中）。
 * 模拟器无此机制（各处直接 pthread）。 */
int ui_bg_task_post(void *(*fn)(void *), void *arg);

void ui_init(void);

/* 回到桌面 */
void ui_show_launcher(void);

/* 打开指定 App 页面 */
void ui_open_app(int app_id);

/* 各页面构建器（app_manager 注册用；screen 由 app_manager 预建） */
void ui_weather_build(lv_obj_t *screen, int app_id); /* 天气（Open-Meteo） */
void ui_weather_on_exit(void);
/* 联网后补拉 Web 下发配置（主题/背景/按键/主页排布）；main 在 GOT_IP
 * 后用大栈任务先后调：prefetch 抓网络（不持锁、不碰 LVGL），refresh
 * 持显示锁应用结果（会改主题/按键映射并重建桌面） */
void ui_remote_config_prefetch(void);
void ui_remote_config_refresh(void);
void ui_conversation_build(lv_obj_t *screen, int app_id);
/* 自检：打开 AstrBot 页的选择机器人面板（无需触摸） */
void ui_conversation_debug_bot_picker(void);
void ui_conversation_on_exit(void);
void ui_files_build(lv_obj_t *screen, int app_id);
void ui_files_on_exit(void);
void ui_pomodoro_build(lv_obj_t *screen, int app_id);
void ui_music_build(lv_obj_t *screen, int app_id);
void ui_music_on_exit(void);
void ui_reader_build(lv_obj_t *screen, int app_id);
void ui_reader_on_exit(void);
void ui_photo_build(lv_obj_t *screen, int app_id);
void ui_photo_on_exit(void);
void ui_pomodoro_on_exit(void);
void ui_settings_build(lv_obj_t *screen, int app_id);
void ui_settings_on_exit(void);
void ui_settings_build_page(lv_obj_t *screen); /* 分类子页重建（nav 用） */

#endif /* UI_H */
