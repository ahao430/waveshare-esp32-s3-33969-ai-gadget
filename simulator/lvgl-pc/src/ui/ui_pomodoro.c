/* ui_pomodoro — 番茄钟 App（2026-11-16 用户需求）。
 *
 * 番茄工作法：25 分钟专注 → 5 分钟休息，自动循环；纯色扁平、大号
 * 倒计时（96px 数字专用字体），只保留开始/暂停、重置、跳过三个操作。
 * 计时用 lv_tick 截止时间（200ms 轮询），不按 tick 次数累加，避免漂移；
 * 切到其他 App 时暂停并保留剩余时间，重新进入显示"继续"。
 * 不做后台计时：设备单前台 App，退出即暂停是产品选择（省电、可预期）。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fonts/ui_font_cn.h"
#include "ui.h"
#include "ui_gesture.h"
#include "ui_theme.h"

#define FOCUS_SEC (25 * 60)
#define BREAK_SEC (5 * 60)
#define TICK_MS   200

/* 纯色扁平配色：专注=番茄红，休息=深绿；文字统一白色系 */
#define BG_FOCUS  0xB23A2E
#define BG_BREAK  0x1E6E55
#define BTN_FOCUS 0x7E2419 /* 主按钮文字色（专注） */
#define BTN_BREAK 0x14533F /* 主按钮文字色（休息） */

static int g_phase;                  /* 0=专注 1=休息 */
static int g_remaining = FOCUS_SEC;  /* 剩余秒数（静态保留，退出不丢） */
static bool g_running;
static int g_done;                   /* 本次运行完成的专注轮数 */
static uint32_t g_deadline;          /* 运行中：剩余归零的 lv_tick */
static int g_shown_sec = -1;         /* 已渲染的秒数（避免每秒重复刷） */

static lv_timer_t *g_timer;
static lv_obj_t *g_root, *g_phase_lbl, *g_time_lbl, *g_done_lbl, *g_hint_lbl;
static lv_obj_t *g_progress, *g_btn_start, *g_lbl_start;

static int phase_total(void) { return g_phase == 0 ? FOCUS_SEC : BREAK_SEC; }
static uint32_t phase_bg(void) { return g_phase == 0 ? BG_FOCUS : BG_BREAK; }
static uint32_t phase_ink(void) { return g_phase == 0 ? BTN_FOCUS : BTN_BREAK; }

/* 运行中按截止时间取剩余秒（向上取整：开始瞬间显示满时长） */
static int remaining_now(void)
{
    int32_t left = (int32_t)(g_deadline - lv_tick_get());
    return left > 0 ? (left + 999) / 1000 : 0;
}

static void update_ui(bool phase_changed)
{
    char buf[32];
    if (phase_changed && g_root) {
        lv_obj_set_style_bg_color(g_root, lv_color_hex(phase_bg()), 0);
        lv_obj_set_style_bg_color(g_btn_start, lv_color_hex(0xFFF7F2), 0);
        lv_obj_set_style_text_color(g_lbl_start, lv_color_hex(phase_ink()), 0);
        lv_label_set_text(g_phase_lbl, g_phase == 0 ? "专注时间 · 25 分钟"
                                                    : "休息时间 · 5 分钟");
        lv_label_set_text(g_hint_lbl, g_phase == 0 ? "保持专注，暂时离开消息"
                                                   : "起身活动一下，喝口水");
        lv_bar_set_range(g_progress, 0, phase_total());
    }
    if (g_time_lbl) {
        snprintf(buf, sizeof(buf), "%02d:%02d", g_remaining / 60, g_remaining % 60);
        lv_label_set_text(g_time_lbl, buf);
    }
    if (g_progress) {
        lv_bar_set_value(g_progress, g_remaining, LV_ANIM_OFF);
    }
    if (g_done_lbl) {
        snprintf(buf, sizeof(buf), "本次完成 %d 个番茄", g_done);
        lv_label_set_text(g_done_lbl, buf);
    }
    if (g_lbl_start) {
        lv_label_set_text(g_lbl_start,
                          g_running ? "暂停"
                                    : (g_remaining == phase_total() ? "开始" : "继续"));
    }
    g_shown_sec = g_remaining;
}

static void next_phase(bool completed)
{
    if (completed && g_phase == 0) g_done++;
    g_phase ^= 1;
    g_remaining = phase_total();
    /* 完成自然衔接下一阶段；手动跳过沿用当前运行/暂停状态 */
    if (g_running) g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
    update_ui(true);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_running) return;
    int sec = remaining_now();
    if (sec != g_shown_sec) {
        g_remaining = sec;
        update_ui(false);
    }
    if (sec == 0) next_phase(true);
}

static void start_pause_cb(lv_event_t *e)
{
    (void)e;
    if (g_running) {
        g_remaining = remaining_now();
        g_running = false;
    } else {
        if (g_remaining <= 0) g_remaining = phase_total();
        g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
        g_running = true;
    }
    update_ui(false);
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    g_running = false;
    g_remaining = phase_total();
    update_ui(false);
}

static void skip_cb(lv_event_t *e)
{
    (void)e;
    next_phase(false);
}

/* 白色半透明幽灵按钮（重置/跳过） */
static lv_obj_t *ghost_btn(lv_obj_t *scr, const char *txt, lv_coord_t x)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 110, 42);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -18);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_radius(b, 21, 0);
    ui_label(b, txt, &lv_font_cn_14, UI_TEXT);
    return b;
}

void ui_pomodoro_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    g_root = scr;
    g_phase_lbl = g_time_lbl = g_done_lbl = g_hint_lbl = NULL;
    g_progress = g_btn_start = g_lbl_start = NULL;

    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(phase_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_gesture_back_add(scr);

    g_phase_lbl = ui_label(scr, "", &lv_font_cn_16, UI_TEXT);
    lv_obj_set_style_text_opa(g_phase_lbl, LV_OPA_80, 0);
    lv_obj_align(g_phase_lbl, LV_ALIGN_TOP_MID, 0, 66);

    /* 大屏倒计时：96px 数字专用字体（仅 0-9 与冒号，体积很小） */
    g_time_lbl = ui_label(scr, "25:00", &font_digits_96, UI_TEXT);
    lv_obj_set_style_text_letter_space(g_time_lbl, 2, 0);
    lv_obj_align(g_time_lbl, LV_ALIGN_TOP_MID, 0, 112);

    g_progress = lv_bar_create(scr);
    lv_obj_set_size(g_progress, 300, 6);
    lv_obj_align(g_progress, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_set_style_radius(g_progress, 3, 0);
    lv_obj_set_style_bg_color(g_progress, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_20, 0);
    lv_obj_set_style_border_width(g_progress, 0, 0);
    lv_bar_set_range(g_progress, 0, phase_total());
    lv_obj_set_style_bg_color(g_progress, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_90, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_progress, 3, LV_PART_INDICATOR);

    g_done_lbl = ui_label(scr, "本次完成 0 个番茄", &lv_font_cn_12, UI_TEXT);
    lv_obj_set_style_text_opa(g_done_lbl, LV_OPA_70, 0);
    lv_obj_align(g_done_lbl, LV_ALIGN_TOP_MID, 0, 262);

    g_hint_lbl = ui_label(scr, "", &lv_font_cn_12, UI_TEXT);
    lv_obj_set_style_text_opa(g_hint_lbl, LV_OPA_70, 0);
    lv_obj_align(g_hint_lbl, LV_ALIGN_TOP_MID, 0, 292);

    /* 主按钮：开始/暂停（白色实心，文字用阶段深色保证对比度） */
    g_btn_start = lv_button_create(scr);
    lv_obj_set_size(g_btn_start, 200, 56);
    lv_obj_align(g_btn_start, LV_ALIGN_BOTTOM_MID, 0, -84);
    lv_obj_set_style_radius(g_btn_start, 28, 0);
    lv_obj_set_style_bg_color(g_btn_start, lv_color_hex(0xFFF7F2), 0);
    lv_obj_set_style_border_width(g_btn_start, 0, 0);
    g_lbl_start = ui_label(g_btn_start, "开始", &lv_font_cn_20, lv_color_hex(phase_ink()));
    lv_obj_center(g_lbl_start);

    lv_obj_t *b_reset = ghost_btn(scr, "重置", -62);
    lv_obj_add_event_cb(b_reset, reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b_skip = ghost_btn(scr, "跳过", 62);
    lv_obj_add_event_cb(b_skip, skip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_btn_start, start_pause_cb, LV_EVENT_CLICKED, NULL);

    if (g_timer) lv_timer_delete(g_timer);
    g_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    update_ui(true);

    /* 回归钩子：GADGET_DEMO_POMO=1 启动即开始（headless 截帧验证运行态）；
 * =N（<60）把当前阶段剩余设为 N 秒，用于验证阶段切换/完成计数。 */
    const char *demo = getenv("GADGET_DEMO_POMO");
    if (demo) {
        int s = atoi(demo);
        if (s > 0 && s < 60) g_remaining = s;
        g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
        g_running = true;
        update_ui(false);
    }
}

void ui_pomodoro_on_exit(void)
{
    if (g_timer) {
        lv_timer_delete(g_timer);
        g_timer = NULL;
    }
    if (g_running) { /* 离开即暂停，保留剩余时间 */
        g_remaining = remaining_now();
        g_running = false;
    }
    g_root = g_phase_lbl = g_time_lbl = g_done_lbl = g_hint_lbl = NULL;
    g_progress = g_btn_start = g_lbl_start = NULL;
    g_shown_sec = -1;
}