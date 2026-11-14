/* ui_settings — 系统设置页（PLAN §5.1 Settings / §13 息屏档位）：
 * 显示亮度 / 音量 / 息屏超时。设置存 data_source 内存态
 * （真机走 device_config NVS + 设备 config API，Phase 6）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_theme.h"

static void slider_changed_cb(lv_event_t *e)
{
    const char *which = lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    const gadget_state_t *s = ds_state();
    int timeout = s->display_timeout_s;

    if (which[0] == 'b') {
        ds_config_update(v, -1, 0);
    } else {
        ds_config_update(0, v, 0);
    }
    (void)timeout;

    /* 值标签由调用方持有 user_data 之外的第二引用——简化：刷新整页值
     * 标签走静态引用 */
    extern lv_obj_t *g_settings_bright_val;
    extern lv_obj_t *g_settings_vol_val;
    char buf[16];
    if (which[0] == 'b' && g_settings_bright_val) {
        snprintf(buf, sizeof(buf), "%d%%", v);
        lv_label_set_text(g_settings_bright_val, buf);
    }
    if (which[0] == 'v' && g_settings_vol_val) {
        snprintf(buf, sizeof(buf), "%d%%", v);
        lv_label_set_text(g_settings_vol_val, buf);
    }
}

/* 全屏二维码：模态遮罩 + 白底大码 + URL 文字；点击遮罩关闭 */
static void qr_close_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    lv_obj_delete(mask);
}

static void qr_open_fullscreen(const char *url)
{
    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mask, 480, 480);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mask, qr_close_cb, LV_EVENT_CLICKED, mask);

    lv_obj_t *qr = lv_qrcode_create(mask);
    lv_qrcode_update(qr, url, (uint32_t)strlen(url));
    lv_obj_set_size(qr, 320, 320);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 50);

    char shown[96];
    snprintf(shown, sizeof(shown), "%s", url);
    lv_obj_t *lbl = ui_label(mask, shown, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 396);

    lv_obj_t *hint = ui_label(mask, "点击任意处关闭", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void refresh_changed_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    static const int vals[] = { 300, 900, 1800, 3600 };
    int idx = (int)lv_dropdown_get_selected(dd);
    if (idx >= 0 && idx < 4) {
        ds_config_set_refresh_interval(vals[idx]);
    }
}

static void qr_open_fullscreen_cb(lv_event_t *e)
{
    qr_open_fullscreen((const char *)lv_event_get_user_data(e));
}

static void timeout_changed_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    int idx = (int)lv_dropdown_get_selected(dd);
    static const int timeouts[] = { 15, 30, 60, 120, 300 };
    if (idx >= 0 && idx < 5) {
        ds_config_update(0, 0, timeouts[idx]);
    }
}

lv_obj_t *g_settings_bright_val;
lv_obj_t *g_settings_vol_val;

void ui_settings_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    lv_obj_t *title_lbl = ui_label(scr, "系统设置", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    app_manager_back_on_click(back);
    lv_obj_t *icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(icon);

    const gadget_state_t *s = ds_state();
    char buf[16];

    /* ---- 显示卡 ---- */
    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, LV_PCT(100), 82);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 50);
    ui_style_card(card1);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(card1, "显示亮度", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(ui_label(card1, "", &lv_font_montserrat_12, UI_TEXT_DIM),
                 LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *bval = ui_label(card1, "", &lv_font_montserrat_14, UI_ACCENT2);
    lv_obj_align(bval, LV_ALIGN_TOP_RIGHT, 0, 0);
    g_settings_bright_val = bval;
    snprintf(buf, sizeof(buf), "%d%%", s->brightness);
    lv_label_set_text(bval, buf);

    lv_obj_t *bslider = lv_slider_create(card1);
    lv_obj_set_size(bslider, LV_PCT(100) - 8, 10);
    lv_obj_align(bslider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(bslider, 10, 100);
    lv_slider_set_value(bslider, s->brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bslider, UI_CARD_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bslider, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(bslider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(bslider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)"b");

    /* ---- 音量卡 ---- */
    lv_obj_t *card2 = lv_obj_create(scr);
    lv_obj_set_size(card2, LV_PCT(100), 82);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 138);
    ui_style_card(card2);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(card2, "扬声器音量", &lv_font_cn_14, UI_TEXT);
    lv_obj_t *vval = ui_label(card2, "", &lv_font_montserrat_14, UI_ACCENT2);
    lv_obj_align(vval, LV_ALIGN_TOP_RIGHT, 0, 0);
    g_settings_vol_val = vval;
    snprintf(buf, sizeof(buf), "%d%%", s->volume);
    lv_label_set_text(vval, buf);

    lv_obj_t *vslider = lv_slider_create(card2);
    lv_obj_set_size(vslider, LV_PCT(100) - 8, 10);
    lv_obj_align(vslider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(vslider, 0, 100);
    lv_slider_set_value(vslider, s->volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vslider, UI_CARD_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vslider, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(vslider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(vslider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)"v");

    /* ---- 息屏卡 ---- */
    lv_obj_t *card3 = lv_obj_create(scr);
    lv_obj_set_size(card3, LV_PCT(100), 66);
    lv_obj_align(card3, LV_ALIGN_TOP_MID, 0, 226);
    ui_style_card(card3);
    lv_obj_clear_flag(card3, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(card3, "自动息屏", &lv_font_cn_14, UI_TEXT);
    static const int timeouts[] = { 15, 30, 60, 120, 300 };
    int cur = s->display_timeout_s;
    int sel = 2;
    for (int i = 0; i < 5; i++) {
        if (timeouts[i] == cur) sel = i;
    }
    lv_obj_t *dd = lv_dropdown_create(card3);
    lv_dropdown_set_options(dd, "15 秒\n30 秒\n1 分钟\n2 分钟\n5 分钟");
    lv_dropdown_set_selected(dd, (uint16_t)sel);
    lv_obj_set_size(dd, 150, 40);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(dd, &lv_font_cn_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(dd, &lv_font_cn_14, 0);
    lv_obj_add_event_cb(dd, timeout_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- 订阅刷新间隔卡（PLAN §6.3：默认 5~15 分钟） ---- */
    lv_obj_t *card5 = lv_obj_create(scr);
    lv_obj_set_size(card5, LV_PCT(100), 66);
    lv_obj_align(card5, LV_ALIGN_TOP_MID, 0, 298);
    ui_style_card(card5);
    lv_obj_clear_flag(card5, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(card5, "订阅自动刷新", &lv_font_cn_14, UI_TEXT);
    lv_obj_t *rdd = lv_dropdown_create(card5);
    lv_dropdown_set_options(rdd, "5 分钟\n15 分钟\n30 分钟\n60 分钟");
    const gadget_state_t *st2 = ds_state();
    int rsel = 1;
    const int rvals[] = { 300, 900, 1800, 3600 };
    for (int i = 0; i < 4; i++) {
        if (rvals[i] == st2->sub_refresh_interval_s) rsel = i;
    }
    lv_dropdown_set_selected(rdd, (uint16_t)rsel);
    lv_obj_set_size(rdd, 150, 40);
    lv_obj_align(rdd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(rdd, &lv_font_cn_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(rdd, &lv_font_cn_14, 0);
    lv_obj_add_event_cb(rdd, refresh_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- 网络/配网卡（PLAN §9.3）：链接行 + 点击全屏大码 ---- */
    lv_obj_t *card4 = lv_obj_create(scr);
    lv_obj_set_size(card4, LV_PCT(100), 106);
    lv_obj_align(card4, LV_ALIGN_TOP_MID, 0, 370);
    ui_style_card(card4);
    lv_obj_clear_flag(card4, LV_OBJ_FLAG_SCROLLABLE);

    /* 全屏二维码弹窗（大码可扫；点击任意处关闭） */
    lv_obj_t *row1 = lv_button_create(card4);
    lv_obj_set_size(row1, LV_PCT(100) - 8, 30);
    lv_obj_align(row1, LV_ALIGN_TOP_MID, 0, 4);
    ui_style_card(row1);
    lv_obj_set_style_bg_color(row1, UI_CARD_HI, 0);
    lv_obj_add_event_cb(row1, qr_open_fullscreen_cb, LV_EVENT_CLICKED, (void *)"http://192.168.4.1");
    lv_obj_t *r1t = ui_label(row1, "配网页  http://192.168.4.1", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(r1t, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *r1a = ui_label(row1, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(r1a, LV_ALIGN_RIGHT_MID, -10, 0);

    lv_obj_t *row2 = lv_button_create(card4);
    lv_obj_set_size(row2, LV_PCT(100) - 8, 30);
    lv_obj_align(row2, LV_ALIGN_TOP_MID, 0, 38);
    ui_style_card(row2);
    lv_obj_set_style_bg_color(row2, UI_CARD_HI, 0);
    lv_obj_add_event_cb(row2, qr_open_fullscreen_cb, LV_EVENT_CLICKED, (void *)"http://192.168.1.42");
    lv_obj_t *r2t = ui_label(row2, "管理后台  http://192.168.1.42", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(r2t, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *r2a = ui_label(row2, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(r2a, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 小智官方后台跳转（音色/角色提示词在官方 console 配置） */
    lv_obj_t *row3 = lv_button_create(card4);
    lv_obj_set_size(row3, LV_PCT(100) - 8, 30);
    lv_obj_align(row3, LV_ALIGN_TOP_MID, 0, 72);
    ui_style_card(row3);
    lv_obj_set_style_bg_color(row3, UI_CARD_HI, 0);
    lv_obj_add_event_cb(row3, qr_open_fullscreen_cb, LV_EVENT_CLICKED, (void *)"https://xiaozhi.me/console");
    lv_obj_t *r3t = ui_label(row3, "小智后台  xiaozhi.me/console", &lv_font_cn_14, UI_ACCENT2);
    lv_obj_align(r3t, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *r3a = ui_label(row3, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(r3a, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 演示钩子：GADGET_DEMO_QR=1 自动打开全屏二维码 */
    if (getenv("GADGET_DEMO_QR") && getenv("GADGET_DEMO_QR")[0] == '1') {
        qr_open_fullscreen("http://192.168.4.1");
    }


}
