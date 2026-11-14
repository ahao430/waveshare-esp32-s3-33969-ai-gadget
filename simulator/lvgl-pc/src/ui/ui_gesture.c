/* ui_gesture — 实现，接口见 ui_gesture.h。 */
#include "ui_gesture.h"

#include "app_manager.h"
#include "ui_theme.h"

/* 上滑返回（原厂范式：二级页无返回按钮） */
static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        app_manager_back();
    }
}

void ui_gesture_back_add(lv_obj_t *scr)
{
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
}

static void gesture_ex_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        void (*cb)(void *) = (void (*)(void *))lv_event_get_user_data(e);
        if (cb) cb(NULL);
    }
}

void ui_gesture_back_add_ex(lv_obj_t *scr, void (*cb)(void *), void *user)
{
    lv_obj_add_event_cb(scr, gesture_ex_cb, LV_EVENT_GESTURE, (void *)cb);
    (void)user;
}

void ui_dots_create(lv_obj_t *x, lv_obj_t *dots[], int max_dots, int count,
                    lv_align_t align, int yofs)
{
    for (int i = 0; i < max_dots; i++) {
        if (dots[i]) { lv_obj_delete(dots[i]); dots[i] = NULL; }
    }
    if (count > max_dots) count = max_dots;
    if (count <= 0) return;
    for (int ui_dots_i = 0; ui_dots_i < count; ui_dots_i++) {
        lv_obj_t *d = lv_obj_create(x);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, UI_CARD_HI, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, align, -((count - 1) * 18) / 2 + ui_dots_i * 18, yofs);
        dots[ui_dots_i] = d;
    }
}

void ui_dots_update(lv_obj_t *dots[], int count, int cur)
{
    for (int i = 0; i < count && dots[i]; i++) {
        bool on = (i == cur);
        lv_obj_set_size(dots[i], on ? 16 : 8, 8);
        lv_obj_set_style_bg_color(dots[i], on ? UI_ACCENT : UI_CARD_HI, 0);
    }
}

int ui_page_of_scroll(lv_obj_t *scrollable, int page_w)
{
    int sx = lv_obj_get_scroll_x(scrollable);
    return sx / page_w; /* 右滑 → scroll_x 增 */
}
