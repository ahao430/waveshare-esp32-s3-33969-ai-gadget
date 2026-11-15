/* ui_statusbar — 实现，接口说明见 ui_statusbar.h。 */
#include <stdio.h>

#include "data_source.h"
#include "ui_theme.h"
#include "ui_statusbar.h"

static lv_obj_t *g_bar;
static lv_obj_t *g_right;      /* 右侧图标簇（flex 行，右对齐） */
static lv_obj_t *g_lbl_wifi;
static lv_obj_t *g_lbl_batt;   /* 电池五档图标 / USB 图标 */
static lv_obj_t *g_lbl_bolt;   /* 充电闪电（不充电时隐藏） */
static lv_obj_t *g_lbl_pct;    /* "86%"（无电池时显示 "USB"） */

static lv_timer_t *g_timer;

/* 电量 → 五档图标（LVGL FontAwesome 电池组） */
static const char *batt_icon(int pct)
{
    if (pct >= 80) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60) return LV_SYMBOL_BATTERY_3;
    if (pct >= 40) return LV_SYMBOL_BATTERY_2;
    if (pct >= 20) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* 电量 → 颜色（20/10 阈值，PLAN §13.3 规则表） */
static lv_color_t batt_color(int pct)
{
    if (pct < 10) return UI_ERR;
    if (pct < 20) return UI_WARN;
    return UI_TEXT_DIM;
}

void ui_statusbar_refresh(void)
{
    if (!g_bar) return;
    const gadget_state_t *s = ds_state();

    lv_obj_set_style_text_color(g_lbl_wifi, s->wifi_ok ? UI_TEXT_DIM : UI_ERR, 0);

    if (!s->battery_present) {
        /* USB 供电、未检到电池：USB 图标 + "USB"，不显示虚假 100% */
        lv_label_set_text(g_lbl_batt, LV_SYMBOL_USB);
        lv_obj_set_style_text_color(g_lbl_batt, UI_TEXT_DIM, 0);
        lv_label_set_text(g_lbl_pct, s->usb_connected ? "USB" : "--");
        lv_obj_set_style_text_color(g_lbl_pct, UI_TEXT_DIM, 0);
        lv_obj_add_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(g_lbl_batt, batt_icon(s->battery_pct));
    lv_obj_set_style_text_color(g_lbl_batt, batt_color(s->battery_pct), 0);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", s->battery_pct);
    lv_label_set_text(g_lbl_pct, buf);
    lv_obj_set_style_text_color(g_lbl_pct, batt_color(s->battery_pct), 0);

    if (s->charging) {
        lv_obj_clear_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);
    }
}

static void statusbar_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_statusbar_refresh();
}

void ui_statusbar_init(void)
{
    if (g_bar) return;

    g_bar = lv_obj_create(lv_layer_top());
    /* 真机目视反馈"状态栏小"：24→34px，图标 14→20、百分比 12→16
     *（百分比一直有，12px 在小屏看不清） */
    lv_obj_set_size(g_bar, 480, 34);
    lv_obj_align(g_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_bar, 0, 0);
    lv_obj_set_style_pad_all(g_bar, 0, 0);
    lv_obj_clear_flag(g_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_bar, LV_OBJ_FLAG_CLICKABLE); /* 点击穿透到下层屏幕 */

    /* 右侧图标簇：flex 行 [Wi-Fi][充电][电池][百分比]，整体右缩 20px。
     * 圆屏顶部安全区（同桌面时间卡左缩 10）：百分比原来贴 x=480 被圆角
     * 裁掉，右侧必须留白；电池与百分比只隔 4px 视为一组，充电图标在电池
     * 左侧（不充电时隐藏，flex 自动跳过不留空档）。
     * 固定宽 160：内容自适应宽会和 align 互相触发重排，且百分比位数变化
     * 会整簇抖动；固定盒 + MAIN_END 只让数字向左长。 */
    g_right = lv_obj_create(g_bar);
    lv_obj_set_size(g_right, 160, 28);
    lv_obj_align(g_right, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_opa(g_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_right, 0, 0);
    lv_obj_set_style_pad_all(g_right, 0, 0);
    lv_obj_clear_flag(g_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(g_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    g_lbl_wifi = ui_label(g_right, LV_SYMBOL_WIFI, &lv_font_montserrat_20, UI_TEXT_DIM);

    g_lbl_bolt = ui_label(g_right, LV_SYMBOL_CHARGE, &lv_font_montserrat_16, UI_OK);
    lv_obj_set_style_margin_left(g_lbl_bolt, 12, 0);
    lv_obj_add_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);

    g_lbl_batt = ui_label(g_right, LV_SYMBOL_BATTERY_FULL, &lv_font_montserrat_20,
                          UI_TEXT_DIM);
    lv_obj_set_style_margin_left(g_lbl_batt, 12, 0);

    g_lbl_pct = ui_label(g_right, "--%", &lv_font_montserrat_16, UI_TEXT_DIM);
    /* -4：抵消两个字形盒的左右留白，让"电池 + 数字"看起来是一组 */
    lv_obj_set_style_margin_left(g_lbl_pct, -4, 0);

    g_timer = lv_timer_create(statusbar_timer_cb, 5000, NULL);
    (void)g_timer;
    ui_statusbar_refresh();
}
