/* ui_settings — 系统设置页（PLAN §5.1 Settings / §13 息屏档位）：
 * 显示亮度 / 音量 / 息屏超时。设置存 data_source 内存态
 * （真机走 device_config NVS + 设备 config API，Phase 6）。
 */
#include <stdio.h>

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
    lv_obj_set_size(card1, LV_PCT(100), 96);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 56);
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
    lv_obj_set_size(card2, LV_PCT(100), 96);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 158);
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
    lv_obj_set_size(card3, LV_PCT(100), 72);
    lv_obj_align(card3, LV_ALIGN_TOP_MID, 0, 260);
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

    /* ---- 网络/配网卡（PLAN §9.3：屏幕显示入口，二维码只含 URL） ---- */
    lv_obj_t *card4 = lv_obj_create(scr);
    lv_obj_set_size(card4, LV_PCT(100), 134);
    lv_obj_align(card4, LV_ALIGN_TOP_MID, 0, 338);
    ui_style_card(card4);
    lv_obj_clear_flag(card4, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(card4, "配网与后台", &lv_font_cn_14, UI_TEXT);

    /* 配网二维码（连接设备热点后扫码打开 192.168.4.1） */
    lv_obj_t *qr1 = lv_qrcode_create(card4);
    lv_qrcode_update(qr1, "http://192.168.4.1", 18); /* 9.2：第三参=数据长度 */
    lv_obj_set_size(qr1, 60, 60);
    lv_obj_align(qr1, LV_ALIGN_LEFT_MID, 12, -4);
    lv_obj_t *l1 = ui_label(card4, "配网入口", &lv_font_cn_12, UI_ACCENT2);
    lv_obj_align(l1, LV_ALIGN_LEFT_MID, 84, -22);
    lv_obj_t *l2 = ui_label(card4, "连接设备热点后\n扫码打开配网页", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(l2, LV_ALIGN_LEFT_MID, 84, 8);

    /* 局域网管理二维码 */
    lv_obj_t *qr2 = lv_qrcode_create(card4);
    lv_qrcode_update(qr2, "http://192.168.1.42", 18);
    lv_obj_set_size(qr2, 60, 60);
    lv_obj_align(qr2, LV_ALIGN_RIGHT_MID, -12, -4);
    lv_obj_t *l3 = ui_label(card4, "管理后台", &lv_font_cn_12, UI_ACCENT2);
    lv_obj_align(l3, LV_ALIGN_RIGHT_MID, -84, -22);
    lv_obj_t *l4 = ui_label(card4, "连接家庭 Wi-Fi 后\n扫码打开管理网页", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(l4, LV_ALIGN_RIGHT_MID, -84, 8);

    lv_obj_t *note = ui_label(scr, "设置暂存内存 · 真机写入 NVS（Phase 6）",
                              &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, 2);
}
