/* ui_gesture — 实现，接口见 ui_gesture.h。 */
#include "ui_gesture.h"

#include "app_manager.h"
#include "ui_theme.h"

/* ---- 上滑返回（自研快甩检测，不依赖 LVGL 手势系统） ----
 * 真机排障记录：LV_EVENT_GESTURE 由 indev 发给「按下的对象」（详情页里
 * 是内容卡片/label），挂在 screen 根上的回调收不到；滚动容器接管后
 * lv_indev_get_gesture_dir 也不再报 LV_DIR_TOP（开 LV_USE_FLOAT +
 * GESTURE_RECOGNITION 实测无效）。改为 30ms 轮询指针 indev 的 proc
 * 轨迹，RELEASED 时按「快甩」判定——dy 大且时间短、纵向主导，
 * 与页面慢拖滚动天然区分：慢拖=滚动，快甩=返回。 */

#define GB_MAX_SCREENS 6
typedef struct {
    lv_obj_t *scr;
    void (*cb)(void *);
    void (*flick)(void *user, int dir);
    void *user;
} gb_entry_t;
static gb_entry_t g_gb[GB_MAX_SCREENS];
static lv_timer_t *g_gb_timer;
static lv_indev_t *g_indev;
static bool g_pressed;
static lv_point_t g_start, g_last;
static uint32_t g_t0;
static uint32_t g_last_fire;

/* 快甩阈值：480px 圆屏上 70px/400ms 相当温和，纵向 2:1 排除斜滑误触 */
#define GB_SWIPE_MIN_DY 70
#define GB_SWIPE_MAX_MS 400
#define GB_SWIPE_DOMINANCE 2

static void gb_fire(int dir)
{
    lv_obj_t *scr = lv_screen_active();
    for (int i = 0; i < GB_MAX_SCREENS; i++) {
        if (g_gb[i].scr != scr) continue;
        if (g_gb[i].flick) {
            g_gb[i].flick(g_gb[i].user, dir);
        } else if (dir == UI_GESTURE_UP && g_gb[i].cb) {
            g_gb[i].cb(g_gb[i].user);
        }
        return;
    }
}

static void gb_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_indev) {
        /* 第一个指针类 indev：9.x 触摸/鼠标统一 LV_INDEV_TYPE_POINTER，
         * 真机=触摸，模拟器=SDL 鼠标（行为一致）。
         * 只用公开 API（9.2 起 lv_indev_t 结构私有化） */
        for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
            if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
                g_indev = i;
                break;
            }
        }
        if (!g_indev) return;
    }

    if (lv_indev_get_state(g_indev) == LV_INDEV_STATE_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(g_indev, &p);
        if (!g_pressed) {
            g_pressed = true;
            g_start = g_last = p;
            g_t0 = lv_tick_get();
        } else {
            g_last = p;
        }
    } else if (g_pressed) {
        /* RELEASED 结算 */
        g_pressed = false;
        int32_t dx = g_last.x - g_start.x;
        int32_t dy = g_last.y - g_start.y;
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;
        uint32_t dt = lv_tick_elaps(g_t0);
        int dir = -1;
        if (dt <= GB_SWIPE_MAX_MS) {
            if (dy <= -GB_SWIPE_MIN_DY &&
                -dy >= GB_SWIPE_DOMINANCE * adx)
                dir = UI_GESTURE_UP;
            else if (dy >= GB_SWIPE_MIN_DY &&
                     dy >= GB_SWIPE_DOMINANCE * adx)
                dir = UI_GESTURE_DOWN;
            else if (dx <= -GB_SWIPE_MIN_DY &&
                     -dx >= GB_SWIPE_DOMINANCE * ady)
                dir = UI_GESTURE_LEFT;
            else if (dx >= GB_SWIPE_MIN_DY &&
                     dx >= GB_SWIPE_DOMINANCE * ady)
                dir = UI_GESTURE_RIGHT;
        }
        if (dir >= 0 && lv_tick_elaps(g_last_fire) > 250) {
            g_last_fire = lv_tick_get();
            gb_fire(dir);
        }
    }
}

static void gb_unregister(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_target(e);
    for (int i = 0; i < GB_MAX_SCREENS; i++) {
        if (g_gb[i].scr == scr) g_gb[i].scr = NULL;
    }
}

static void gb_register(lv_obj_t *scr, void (*cb)(void *),
                        void (*flick)(void *user, int dir), void *user)
{
    /* 同屏重复注册：覆盖（back/flick 二选一） */
    int slot = -1;
    bool existed = false;
    for (int i = 0; i < GB_MAX_SCREENS; i++) {
        if (g_gb[i].scr == scr) { slot = i; existed = true; break; }
        if (g_gb[i].scr == NULL && slot < 0) slot = i;
    }
    if (slot < 0) return; /* 表满：详情页等场景不会超 6 屏 */

    g_gb[slot].scr = scr;
    g_gb[slot].cb = cb;
    g_gb[slot].flick = flick;
    g_gb[slot].user = user;
    if (!existed)
        lv_obj_add_event_cb(scr, gb_unregister, LV_EVENT_DELETE, NULL);

    if (!g_gb_timer) g_gb_timer = lv_timer_create(gb_timer_cb, 30, NULL);
}

static void gb_back_cb(void *user)
{
    (void)user;
    app_manager_back();
}

void ui_gesture_back_add(lv_obj_t *scr)
{
    gb_register(scr, gb_back_cb, NULL, NULL);
}

void ui_gesture_back_add_ex(lv_obj_t *scr, void (*cb)(void *), void *user)
{
    gb_register(scr, cb, NULL, user);
}

void ui_gesture_flick_add(lv_obj_t *scr, void (*cb)(void *user, int dir),
                          void *user)
{
    gb_register(scr, NULL, cb, user);
}

/* 页点未选中色：UI_CARD_HI(0x1F2A47) 在暗背景上近不可见（真机目视
 * 反馈"页点太浅"）→ 提亮为中灰蓝（不动全局主题色，页点专用） */
#define UI_DOTS_OFF lv_color_hex(0x46527A)

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
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, UI_DOTS_OFF, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, align, -((count - 1) * 20) / 2 + ui_dots_i * 20, yofs);
        dots[ui_dots_i] = d;
    }
}

void ui_dots_update(lv_obj_t *dots[], int count, int cur)
{
    for (int i = 0; i < count && dots[i]; i++) {
        bool on = (i == cur);
        lv_obj_set_size(dots[i], on ? 18 : 10, 10);
        lv_obj_set_style_bg_color(dots[i], on ? UI_ACCENT : UI_DOTS_OFF, 0);
    }
}

int ui_page_of_scroll(lv_obj_t *scrollable, int page_w)
{
    /* page_w<=0 = 实测步长：tileview 磁贴按 pct 定位（col*100% 内容宽），
     * 步长=内容宽，未必等于屏宽——曾硬编码 480 导致页点错位
     * （tv 宽 448 时第三页 896/480 整除落 1，选中了中间点）。 */
    if (page_w <= 0) {
        page_w = lv_obj_get_content_width(scrollable);
        if (page_w <= 0) return 0;
    }
    int sx = lv_obj_get_scroll_x(scrollable);
    return (sx + page_w / 2) / page_w; /* 四舍五入到最近页（滚动中间态友好） */
}
