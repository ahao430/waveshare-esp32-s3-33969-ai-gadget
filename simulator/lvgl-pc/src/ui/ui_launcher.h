/* ui_launcher — Launcher 页面构建入口。 */
#ifndef UI_LAUNCHER_H
#define UI_LAUNCHER_H

#include "lvgl.h"

/* 构建（幂等）并返回 Launcher 屏幕 */
lv_obj_t *ui_launcher_screen(void);

/* 重建 Launcher（主题/背景切换后调用；时钟定时器随之重建） */
void ui_launcher_rebuild(void);

#endif /* UI_LAUNCHER_H */
