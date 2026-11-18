/* ui_gesture — 交互范式公共件（ADR-008：对齐原厂操作方式）。
 * - 上滑返回：二级页去 back 按钮，上滑手势统一返回
 *   （长按/短按物理键映射见 POWER_MANAGEMENT §13.4）
 * - 页点：桌面/二级页横滑容器的页数指示（原厂一屏四格范式） */
#ifndef UI_GESTURE_H
#define UI_GESTURE_H

#include "lvgl.h"

#define UI_DOTS_MAX 12

/* 二级页上滑返回：注册该屏的「快甩返回」监听（→ app_manager_back）。
 * 所有 app 页面统一调用（替代原左上 back 按钮）。
 * 交互口径：慢拖 = 页面滚动，快甩（≥70px、≤400ms、纵向主导）= 返回。 */
void ui_gesture_back_add(lv_obj_t *scr);

/* 自定义上滑行为（如设置子页上滑 = 回设置首页的层级返回） */
void ui_gesture_back_add_ex(lv_obj_t *scr, void (*cb)(void *), void *user);

/* 方向快甩回调（阅读器等需要四向手势的页面）。
 * 注册后该屏的「上滑返回」由回调自行处理（不再自动 app_manager_back）。
 * dir：UI_GESTURE_UP/DOWN/LEFT/RIGHT（快甩 ≥70px、≤400ms、单轴主导）。 */
enum {
    UI_GESTURE_UP = 0,
    UI_GESTURE_DOWN,
    UI_GESTURE_LEFT,
    UI_GESTURE_RIGHT,
};
void ui_gesture_flick_add(lv_obj_t *scr, void (*cb)(void *user, int dir),
                          void *user);

/* 页点组：count 个点（current 高亮拉伸）。
 * dots 数组由调用方持有（容量 >= count），页切换时调 ui_dots_update。 */
void ui_dots_create(lv_obj_t *parent, lv_obj_t *dots[], int max_dots, int count,
                    lv_align_t align, int yofs);
void ui_dots_update(lv_obj_t *dots[], int count, int cur);

/* 计算横滑容器当前页（0 基）：scroll_x / 页宽 */
int ui_page_of_scroll(lv_obj_t *scrollable, int page_w);

#endif /* UI_GESTURE_H */
