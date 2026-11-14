/* ui_settings — 分类式设置（PRODUCT_SPEC §4，P0）：
 * 设置首页（四入口卡 + 固件状态行）→ 子页（显示与声音 / 网络与连接 /
 * 服务管理 / 设备信息）。子页 back 回首页；首页 back 回 Launcher。
 * 设置存 data_source 内存态；真机走 device_config NVS（Phase 6）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_theme.h"

/* 子页 id */
enum settings_page {
    SP_HOME = 0,
    SP_DISPLAY,   /* 显示与声音 */
    SP_NETWORK,   /* 网络与连接 */
    SP_SERVICE,   /* 服务管理 */
    SP_DEVICE,    /* 设备信息 */
};

static int g_page = SP_HOME;
static lv_obj_t *g_root;
static lv_obj_t *g_poll_timer;

/* ---------------- 全屏二维码（网络页用） ---------------- */

static void qr_close_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    lv_obj_delete(mask);
}

static void qr_open_fullscreen_cb(lv_event_t *e)
{
    const char *url = lv_event_get_user_data(e);

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

    lv_obj_t *lbl = ui_label(mask, url, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 396);

    lv_obj_t *hint = ui_label(mask, "点击任意处关闭", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/* ---------------- 通用控件 ---------------- */

static void nav_cb(lv_event_t *e)
{
    int page = (int)(uintptr_t)lv_event_get_user_data(e);
    g_page = page;
    lv_obj_clean(g_root);
    ui_settings_build_page(g_root);
}

static void back_home_cb(lv_event_t *e)
{
    (void)e;
    g_page = SP_HOME;
    lv_obj_clean(g_root);
    ui_settings_build_page(g_root);
}

static void back_btn_add(lv_obj_t *scr)
{
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_add_event_cb(back, back_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(icon);
}

/* 子页标题：居中，与 back 同行（原实现未 align 会与 back 叠位） */
static void page_title(lv_obj_t *scr, const char *title)
{
    lv_obj_t *t = ui_label(scr, title, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
}

static void slider_changed_cb(lv_event_t *e)
{
    const char *which = lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    if (which[0] == 'b') ds_config_update(v, -1, 0);
    else ds_config_update(0, v, 0);
}

static void timeout_changed_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    static const int vals[] = { 15, 30, 60, 120, 300 };
    int idx = (int)lv_dropdown_get_selected(dd);
    if (idx >= 0 && idx < 5) ds_config_update(0, 0, vals[idx]);
}

static void refresh_changed_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    static const int vals[] = { 300, 900, 1800, 3600 };
    int idx = (int)lv_dropdown_get_selected(dd);
    if (idx >= 0 && idx < 4) ds_config_set_refresh_interval(vals[idx]);
}

static lv_obj_t *entry_card(lv_obj_t *scr, int page, const char *title,
                            const char *desc, const char *symbol, uint32_t color)
{
    lv_obj_t *card = lv_button_create(scr);
    lv_obj_set_size(card, LV_PCT(100), 78);
    ui_style_card(card);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_add_event_cb(card, nav_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)page);

    /* 彩色圆角图标块（与桌面磁贴同视觉语言，定稿图风格） */
    lv_obj_t *ic = lv_obj_create(card);
    lv_obj_set_size(ic, 44, 44);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(ic, 13, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sym = ui_label(ic, symbol, &lv_font_montserrat_16, lv_color_hex(0xFFFFFF));
    lv_obj_center(sym);

    lv_obj_t *t = ui_label(card, title, &lv_font_cn_16, UI_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 60, -12);
    lv_obj_t *d = ui_label(card, desc, &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(d, LV_ALIGN_LEFT_MID, 60, 12);
    lv_obj_t *a = ui_label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(a, LV_ALIGN_RIGHT_MID, -10, 0);
    return card;
}

/* ---------------- 页面：显示与声音 ---------------- */

static void page_display(lv_obj_t *scr)
{
    page_title(scr, "显示与声音");

    const gadget_state_t *s = ds_state();
    char buf[16];

    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, LV_PCT(100), 116);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 56);
    ui_style_card(card1);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(card1, "屏幕亮度", &lv_font_cn_14, UI_TEXT);
    lv_obj_t *bval = ui_label(card1, "", &lv_font_montserrat_14, UI_ACCENT2);
    lv_obj_align(bval, LV_ALIGN_TOP_RIGHT, 0, 0);
    snprintf(buf, sizeof(buf), "%d%%", s->brightness);
    lv_label_set_text(bval, buf);
    lv_obj_t *bs = lv_slider_create(card1);
    lv_obj_set_size(bs, LV_PCT(100) - 8, 10);
    lv_obj_align(bs, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(bs, 10, 100);
    lv_slider_set_value(bs, s->brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bs, UI_CARD_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bs, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(bs, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(bs, slider_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)"b");

    lv_obj_t *card2 = lv_obj_create(scr);
    lv_obj_set_size(card2, LV_PCT(100), 116);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 180);
    ui_style_card(card2);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(card2, "扬声器音量", &lv_font_cn_14, UI_TEXT);
    lv_obj_t *vval = ui_label(card2, "", &lv_font_montserrat_14, UI_ACCENT2);
    lv_obj_align(vval, LV_ALIGN_TOP_RIGHT, 0, 0);
    snprintf(buf, sizeof(buf), "%d%%", s->volume);
    lv_label_set_text(vval, buf);
    lv_obj_t *vs = lv_slider_create(card2);
    lv_obj_set_size(vs, LV_PCT(100) - 8, 10);
    lv_obj_align(vs, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(vs, 0, 100);
    lv_slider_set_value(vs, s->volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vs, UI_CARD_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vs, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(vs, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(vs, slider_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)"v");

    lv_obj_t *card3 = lv_obj_create(scr);
    lv_obj_set_size(card3, LV_PCT(100), 116);
    lv_obj_align(card3, LV_ALIGN_TOP_MID, 0, 304);
    ui_style_card(card3);
    lv_obj_clear_flag(card3, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(card3, "自动息屏", &lv_font_cn_14, UI_TEXT);
    lv_obj_t *dd = lv_dropdown_create(card3);
    lv_dropdown_set_options(dd, "15 秒\n30 秒\n1 分钟\n2 分钟\n5 分钟");
    int sel = 2;
    const int tv[] = { 15, 30, 60, 120, 300 };
    for (int i = 0; i < 5; i++) if (tv[i] == s->display_timeout_s) sel = i;
    lv_dropdown_set_selected(dd, (uint16_t)sel);
    lv_obj_set_size(dd, 170, 44);
    lv_obj_align(dd, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_font(dd, &lv_font_cn_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(dd, &lv_font_cn_14, 0);
    lv_obj_add_event_cb(dd, timeout_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---------------- 页面：网络与连接 ---------------- */

static void page_network(lv_obj_t *scr)
{
    page_title(scr, "网络与连接");

    /* Wi-Fi 状态卡（模拟器 mock；真机读 network_manager） */
    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, LV_PCT(100), 72);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 56);
    ui_style_card(card1);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dot = lv_obj_create(card1);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, UI_OK, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *w = ui_label(card1, "Home-WiFi-5G · 已连接", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(w, LV_ALIGN_LEFT_MID, 22, 0);
    lv_obj_t *ip = ui_label(card1, "192.168.1.42", &lv_font_montserrat_12, UI_TEXT_DIM);
    lv_obj_align(ip, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 三行链接（点击 → 全屏大码） */
    struct { const char *name; const char *url; const char *qr; } rows[] = {
        { "配网页",   "http://192.168.4.1",  "http://192.168.4.1" },
        { "小智后台", "xiaozhi.me/console",  "https://xiaozhi.me/console" },
        { "管理后台", "http://192.168.1.42", "http://192.168.1.42" },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_button_create(scr);
        lv_obj_set_size(row, LV_PCT(100) - 8, 48);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 140 + i * 56);
        ui_style_card(row);
        lv_obj_set_style_bg_color(row, UI_CARD_HI, 0);
        lv_obj_add_event_cb(row, qr_open_fullscreen_cb, LV_EVENT_CLICKED, (void *)rows[i].qr);
        lv_obj_t *t = ui_label(row, rows[i].name, &lv_font_cn_14, UI_TEXT);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *u = ui_label(row, rows[i].url, &lv_font_montserrat_12, UI_TEXT_DIM);
        lv_obj_align(u, LV_ALIGN_RIGHT_MID, -34, 0);
        lv_obj_t *a = ui_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT_DIM);
        lv_obj_align(a, LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

/* ---------------- 页面：服务管理 ---------------- */

static void page_service(lv_obj_t *scr)
{
    page_title(scr, "服务管理");

    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, LV_PCT(100), 116);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 56);
    ui_style_card(card1);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(card1, "订阅自动刷新", &lv_font_cn_14, UI_TEXT);
    const gadget_state_t *s = ds_state();
    lv_obj_t *rdd = lv_dropdown_create(card1);
    lv_dropdown_set_options(rdd, "5 分钟\n15 分钟\n30 分钟\n60 分钟");
    int rsel = 1;
    const int rvals[] = { 300, 900, 1800, 3600 };
    for (int i = 0; i < 4; i++) if (rvals[i] == s->sub_refresh_interval_s) rsel = i;
    lv_dropdown_set_selected(rdd, (uint16_t)rsel);
    lv_obj_set_size(rdd, 170, 44);
    lv_obj_align(rdd, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_font(rdd, &lv_font_cn_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(rdd, &lv_font_cn_14, 0);
    lv_obj_add_event_cb(rdd, refresh_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *note = ui_label(scr,
        "具体刷新周期在「订阅中心」内管理；\n允许手动刷新。自动刷新任务不会\n长期占用网络与电量。",
        &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 190);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 430);
}

/* ---------------- 页面：设备信息 ---------------- */

static void page_device(lv_obj_t *scr)
{
    page_title(scr, "设备信息");

    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, LV_PCT(100), 88);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 56);
    ui_style_card(card1);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dot = lv_obj_create(card1);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, UI_OK, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *fw = ui_label(card1, "Gadget Firmware v0.1.0-sim", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(fw, LV_ALIGN_LEFT_MID, 22, -12);
    lv_obj_t *fw2 = ui_label(card1, "构建 2026-11-14 · 运行正常", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(fw2, LV_ALIGN_LEFT_MID, 22, 14);

    /* 硬件表（规格固定；内存/Flash 动态指标真机采集，PRODUCT_SPEC §4.2） */
    static const char *rows[][2] = {
        { "芯片",  "ESP32-S3" },
        { "CPU",   "双核 LX7 · 240 MHz" },
        { "Flash", "16 MB" },
        { "PSRAM", "8 MB" },
        { "屏幕",  "480 × 480 AMOLED" },
    };
    lv_obj_t *card2 = lv_obj_create(scr);
    lv_obj_set_size(card2, LV_PCT(100), 230);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 152);
    ui_style_card(card2);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *k = ui_label(card2, rows[i][0], &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_align(k, LV_ALIGN_TOP_LEFT, 6, 12 + i * 42);
        lv_obj_t *v = ui_label(card2, rows[i][1], &lv_font_montserrat_14, UI_TEXT);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -6, 10 + i * 42);
    }

    lv_obj_t *note = ui_label(scr,
        "内存/Flash 动态指标真机接入（Phase 6 heap_caps 采集）",
        &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* ---------------- 页面分发 ---------------- */

void ui_settings_build_page(lv_obj_t *scr)
{
    lv_obj_set_style_pad_all(scr, 16, 0);

    switch (g_page) {
    case SP_DISPLAY:
        back_btn_add(scr);
        page_display(scr);
        break;
    case SP_NETWORK:
        back_btn_add(scr);
        page_network(scr);
        break;
    case SP_SERVICE:
        back_btn_add(scr);
        page_service(scr);
        break;
    case SP_DEVICE:
        back_btn_add(scr);
        page_device(scr);
        break;
    case SP_HOME:
    default: {
        /* 首页：无 back（app_manager back → Launcher） */
        lv_obj_t *t = ui_label(scr, "系统设置", &lv_font_cn_20, UI_TEXT);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

        struct { int page; const char *t; const char *d; const char *s; uint32_t c; } items[] = {
            { SP_DISPLAY, "显示与声音", "音量、亮度、息屏",   LV_SYMBOL_SETTINGS, 0x7C5CFF },
            { SP_NETWORK, "网络与连接", "Wi-Fi、配网、二维码", LV_SYMBOL_WIFI,     0x2EC4B6 },
            { SP_SERVICE, "服务管理",   "订阅刷新、后台服务",  LV_SYMBOL_REFRESH,  0xFF8A3D },
            { SP_DEVICE,  "设备信息",   "版本、内存、硬件",    LV_SYMBOL_LIST,     0x3A6FF7 },
        };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *card = entry_card(scr, items[i].page, items[i].t,
                                        items[i].d, items[i].s, items[i].c);
            lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 46 + i * 88);
        }

        lv_obj_t *fw = ui_label(scr, "Gadget Firmware v0.1.0 · 运行正常",
                                &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(fw, LV_ALIGN_BOTTOM_MID, 0, 0);
        break;
    }
    }
}

void ui_settings_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    g_root = scr;
    g_page = SP_HOME;
    ui_settings_build_page(scr);
}

void ui_settings_on_exit(void)
{
    g_page = SP_HOME;
    g_root = NULL;
}
