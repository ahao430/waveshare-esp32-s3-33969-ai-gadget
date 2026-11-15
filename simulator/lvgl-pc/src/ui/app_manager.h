/* app_manager — App 注册/打开/返回/资源释放（PLAN §4.2 系统服务的 PC 版）。
 * 运行模型（PLAN §4.3）：一个业务 App 前台；退出即销毁其屏幕对象，
 * 模拟"非前台 App 释放大块资源"；Launcher（系统 UI）常驻。
 */
#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include <stdbool.h>

#include "lvgl.h"

typedef struct {
    int id;                 /* ui_app_id 枚举 */
    const char *key;        /* 稳定标识（Web 后台排布配置用，如 "chat"） */
    const char *name;       /* 桌面标签 */
    const char *symbol;     /* LV_SYMBOL_* 图标（符号磁贴用） */
    uint32_t tile_color;    /* 磁贴底色（符号磁贴；0 = 无） */
    const char *avatar;     /* 头像图路径（AI 磁贴用；NULL = 符号磁贴） */
    bool enabled;           /* false = 规划中 App，桌面置灰、可进占位页 */
    void (*build)(lv_obj_t *screen, int app_id); /* 在新屏幕上构建页面内容 */
    void (*on_exit)(void);            /* 可 NULL：退出时释放 App 私有资源（定时器等） */
} app_def_t;

/* Launcher 装配后调用一次，登记所有 App（顺序即桌面网格顺序） */
bool app_manager_register(const app_def_t *def);

int app_manager_count(void);
const app_def_t *app_manager_get(int idx); /* 按注册序取；越界返回 NULL */
const app_def_t *app_manager_find(int app_id); /* 按 app id（枚举）查；无则 NULL */

/* 设置常驻 Home 屏幕（Launcher），back 的目的地 */
void app_manager_set_home(lv_obj_t *home);

/* 打开 App：创建新屏幕 → build → 转场进入。
 * 若当前已在某 App 中，先退出（on_exit + 删除屏幕）。 */
void app_manager_open(int app_id);

/* 返回 Launcher；不在 App 中时无操作 */
void app_manager_back(void);

/* 便捷：给按钮挂"点击即返回"回调 */
void app_manager_back_on_click(lv_obj_t *button);

bool app_manager_in_app(void);
int app_manager_current_id(void); /* 不在 App 中返回 -1 */

#endif /* APP_MANAGER_H */
