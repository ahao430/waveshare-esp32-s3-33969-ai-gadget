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
#include "ui_gesture.h"
#include "ui_launcher.h"
#include "ui_theme.h"
#include "ui_wifi_panel.h"

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

    /* 空地址（如未连接 Bridge）：不渲染二维码（0 长度码无意义），给提示 */
    if (url[0] == '\0') {
        lv_obj_t *na = ui_label(mask, "地址未就绪：未连接 Bridge", &lv_font_cn_14, UI_TEXT);
        lv_obj_align(na, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t *hint0 = ui_label(mask, "点击任意处关闭", &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_align(hint0, LV_ALIGN_BOTTOM_MID, 0, -20);
        return;
    }

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

/* 子页上滑 = 回设置首页（层级返回；非退出 App） */
static void swipe_home_cb(void *unused)
{
    (void)unused;
    g_page = SP_HOME;
    lv_obj_clean(g_root);
    ui_settings_build_page(g_root);
}



/* 子页：上滑回设置首页（无返回按钮，ADR-008 范式） */
static void subpage_gesture_add(lv_obj_t *scr)
{
    ui_gesture_back_add_ex(scr, swipe_home_cb, NULL);
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

/* 主题切换：存档 + 改变量 + 重建 Launcher（波浪/磁贴配色）与当前页 */
static void theme_switch_cb(lv_event_t *e)
{
    int dir = (int)(uintptr_t)lv_event_get_user_data(e);
    int idx = (ui_theme_current() + dir + UI_THEME_COUNT) % UI_THEME_COUNT;
    ds_config_set_theme(idx);
    ui_theme_select(idx);
    ui_launcher_rebuild();

    g_page = SP_DISPLAY; /* 重建当前子页（配色即时生效） */
    lv_obj_clean(g_root);
    ui_settings_build_page(g_root);
}

static void page_display(lv_obj_t *scr)
{
    page_title(scr, "显示与声音");

    const gadget_state_t *s = ds_state();
    char buf[16];

    /* 主题色（用户 2026-11-14 需求：多主题可换） */
    lv_obj_t *card0 = lv_obj_create(scr);
    lv_obj_set_size(card0, LV_PCT(100), 64);
    lv_obj_align(card0, LV_ALIGN_TOP_MID, 0, 50);
    ui_style_card(card0);
    lv_obj_clear_flag(card0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tl = ui_label(card0, "主题色", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *tp = lv_button_create(card0);
    lv_obj_set_size(tp, 40, 40);
    lv_obj_align(tp, LV_ALIGN_RIGHT_MID, -96, 0);
    ui_style_card(tp);
    lv_obj_set_style_radius(tp, 12, 0);
    lv_obj_add_event_cb(tp, theme_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)-1);
    lv_obj_t *tpi = ui_label(tp, LV_SYMBOL_LEFT, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(tpi);

    lv_obj_t *tn = ui_label(card0, k_ui_themes[ui_theme_current()].name,
                            &lv_font_cn_16, UI_ACCENT);
    lv_obj_align(tn, LV_ALIGN_RIGHT_MID, -48, 0);

    lv_obj_t *tnx = lv_button_create(card0);
    lv_obj_set_size(tnx, 40, 40);
    lv_obj_align(tnx, LV_ALIGN_RIGHT_MID, 0, 0);
    ui_style_card(tnx);
    lv_obj_set_style_radius(tnx, 12, 0);
    lv_obj_add_event_cb(tnx, theme_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1);
    lv_obj_t *tni = ui_label(tnx, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(tni);

    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, LV_PCT(100), 104);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 122);
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
    lv_obj_set_size(card2, LV_PCT(100), 104);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 236);
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
    lv_obj_set_size(card3, LV_PCT(100), 104);
    lv_obj_align(card3, LV_ALIGN_TOP_MID, 0, 348);
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

/* 面板入口的 event 包装（lv_event_cb_t 签名） */
static void wifi_panel_open_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_panel_open();
}
static void bridge_host_panel_open_cb(lv_event_t *e)
{
    (void)e;
    ui_bridge_host_panel_open();
}

/* 网络页状态轮询（Wi-Fi 状态卡 + Bridge 地址行 + 管理后台 URL 都是
 * 运行期会变的：配网面板连上/保存后这里自动跟上） */
static lv_timer_t *g_net_timer;
static lv_obj_t *g_net_dot;
static lv_obj_t *g_net_ssid_lbl;
static lv_obj_t *g_net_ip_lbl;
static lv_obj_t *g_net_bh_val_lbl;
static lv_obj_t *g_net_admin_val_lbl;
/* 管理后台二维码：Bridge 同源（Web 静态托管在 Bridge 上，B 阶段）；
 * static：二维码回调持有的指针要活过建页栈帧 */
static char g_admin_url[160];

static void net_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_net_ssid_lbl) return;
    char ssid[33], ip[24];
    bool ok = ds_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip));
    lv_obj_set_style_bg_color(g_net_dot, ok ? UI_OK : UI_ERR, 0);
    char line[64];
    snprintf(line, sizeof(line), "%s%s", ok ? ssid : "", ok ? " · 已连接" : "未连接 Wi-Fi");
    lv_label_set_text(g_net_ssid_lbl, line);
    lv_label_set_text(g_net_ip_lbl, ok ? ip : "");
    if (g_net_bh_val_lbl) {
        char bh[160];
        lv_label_set_text(g_net_bh_val_lbl,
                          ds_bridge_host_get(bh, sizeof(bh)) ? bh : "未配置");
    }
    /* 管理后台地址 = Bridge 同源（Web 静态托管在 Bridge 上）；二维码
     * 与行文案随配网改动自动跟上 */
    if (!ds_bridge_host_get(g_admin_url, sizeof(g_admin_url))) {
        g_admin_url[0] = '\0';
    }
    if (g_net_admin_val_lbl) {
        lv_label_set_text(g_net_admin_val_lbl,
                          g_admin_url[0] ? g_admin_url : "未配置 Bridge");
    }
}

static void page_network(lv_obj_t *scr)
{
    page_title(scr, "网络与连接");

    /* Wi-Fi 状态卡（真数据，net_timer_cb 轮询刷新） */
    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, LV_PCT(100), 72);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 52);
    ui_style_card(card1);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);
    g_net_dot = lv_obj_create(card1);
    lv_obj_set_size(g_net_dot, 10, 10);
    lv_obj_set_style_radius(g_net_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_net_dot, UI_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(g_net_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_net_dot, 0, 0);
    lv_obj_align(g_net_dot, LV_ALIGN_LEFT_MID, 4, 0);
    g_net_ssid_lbl = ui_label(card1, "…", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(g_net_ssid_lbl, LV_ALIGN_LEFT_MID, 22, 0);
    g_net_ip_lbl = ui_label(card1, "", &lv_font_montserrat_12, UI_TEXT_DIM);
    lv_obj_align(g_net_ip_lbl, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 行 1：Wi-Fi 设置 → 配网面板（扫描/选网/密码，ADR-009） */
    lv_obj_t *r1 = lv_button_create(scr);
    lv_obj_set_size(r1, LV_PCT(100) - 8, 48);
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 132);
    ui_style_card(r1);
    lv_obj_set_style_bg_color(r1, UI_CARD_HI, 0);
    lv_obj_add_event_cb(r1, wifi_panel_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t1 = ui_label(r1, "Wi-Fi 设置", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(t1, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *h1 = ui_label(r1, LV_SYMBOL_WIFI " 选择网络", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(h1, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 行 2：Bridge 地址 → 编辑面板（保存即 rebind 生效） */
    lv_obj_t *r2 = lv_button_create(scr);
    lv_obj_set_size(r2, LV_PCT(100) - 8, 48);
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 188);
    ui_style_card(r2);
    lv_obj_set_style_bg_color(r2, UI_CARD_HI, 0);
    lv_obj_add_event_cb(r2, bridge_host_panel_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t2 = ui_label(r2, "Bridge 地址", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(t2, LV_ALIGN_LEFT_MID, 10, 0);
    g_net_bh_val_lbl = ui_label(r2, "…", &lv_font_montserrat_12, UI_TEXT_DIM);
    lv_obj_align(g_net_bh_val_lbl, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_label_set_long_mode(g_net_bh_val_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_net_bh_val_lbl, 250);
    lv_obj_t *a2 = ui_label(r2, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(a2, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 行 3/4：二维码行（点击 → 全屏大码） */
    struct { const char *name; const char *url; const char *qr; } qrows[] = {
        { "小智后台", "xiaozhi.me/console", "https://xiaozhi.me/console" },
        { "管理后台", g_admin_url,          g_admin_url },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_button_create(scr);
        lv_obj_set_size(row, LV_PCT(100) - 8, 48);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 244 + i * 56);
        ui_style_card(row);
        lv_obj_set_style_bg_color(row, UI_CARD_HI, 0);
        lv_obj_add_event_cb(row, qr_open_fullscreen_cb, LV_EVENT_CLICKED, (void *)qrows[i].qr);
        lv_obj_t *t = ui_label(row, qrows[i].name, &lv_font_cn_14, UI_TEXT);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *u = ui_label(row, qrows[i].url, &lv_font_montserrat_12, UI_TEXT_DIM);
        lv_obj_align(u, LV_ALIGN_RIGHT_MID, -34, 0);
        if (i == 1) {
            g_net_admin_val_lbl = u; /* timer 刷新（URL 可能在配网后变化） */
        }
        lv_obj_t *a = ui_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT_DIM);
        lv_obj_align(a, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    net_timer_cb(NULL); /* 立即刷一次 */
    g_net_timer = lv_timer_create(net_timer_cb, 1500, NULL);

    /* 回归钩子：进网络页即弹配网面板（headless 截帧验证列表/键盘） */
    if (getenv("GADGET_DEMO_WIFI_PICK")) {
        ui_wifi_panel_open();
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
    lv_obj_set_size(card1, LV_PCT(100), 80);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 52);
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

    /* 电源与电池（PLAN §13.3；模拟器为 mock 演示值，真机 AXP2101 采集） */
    const gadget_state_t *s = ds_state();
    lv_obj_t *pwr = lv_obj_create(scr);
    lv_obj_set_size(pwr, LV_PCT(100), 148);
    lv_obj_align(pwr, LV_ALIGN_TOP_MID, 0, 140);
    ui_style_card(pwr);
    lv_obj_clear_flag(pwr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *pt = ui_label(pwr, "电源与电池", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(pt, LV_ALIGN_TOP_LEFT, 2, 0);

    char vbuf[16], pbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%.2f V", s->battery_voltage_mv / 1000.0);
    snprintf(pbuf, sizeof(pbuf), "%d%%", s->battery_pct);
    if (s->battery_present) {
        lv_obj_t *pv = ui_label(pwr, pbuf, &lv_font_montserrat_20,
                                s->battery_pct < 20 ? UI_WARN : UI_TEXT);
        lv_obj_align(pv, LV_ALIGN_TOP_RIGHT, -2, -4);
        if (s->charging) {
            lv_obj_t *bolt = ui_label(pwr, LV_SYMBOL_CHARGE, &lv_font_montserrat_14, UI_OK);
            lv_obj_align(bolt, LV_ALIGN_TOP_RIGHT, -56, 0);
        }
    } else {
        lv_obj_t *pv = ui_label(pwr, s->usb_connected ? "USB" : "--",
                                &lv_font_montserrat_16, UI_TEXT_DIM);
        lv_obj_align(pv, LV_ALIGN_TOP_RIGHT, -2, 0);
    }

    const char *chg = s->battery_present
                          ? (s->charging ? "充电中" : (s->usb_connected ? "USB 供电" : "电池供电"))
                          : "未检测到电池";
    const char *rows_pwr[][2] = {
        { "电池电压", s->battery_present ? vbuf : "--" },
        { "电池电量", s->battery_present ? pbuf : "--" },
        { "充电状态", chg },
        { "USB 供电", s->usb_connected ? "已连接" : "未连接" },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *k = ui_label(pwr, rows_pwr[i][0], &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(k, LV_ALIGN_TOP_LEFT, 2, 26 + i * 26);
        lv_obj_t *v = ui_label(pwr, rows_pwr[i][1], &lv_font_cn_12, UI_TEXT);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -2, 26 + i * 26);
    }

    /* 硬件表（规格固定；内存/Flash 动态指标真机采集，PRODUCT_SPEC §4.2） */
    static const char *rows[][2] = {
        { "芯片",  "ESP32-S3" },
        { "CPU",   "双核 LX7 · 240 MHz" },
        { "Flash", "16 MB" },
        { "PSRAM", "8 MB" },
        { "屏幕",  "480 × 480 AMOLED" },
    };
    lv_obj_t *card2 = lv_obj_create(scr);
    lv_obj_set_size(card2, LV_PCT(100), 136);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 296);
    ui_style_card(card2);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *k = ui_label(card2, rows[i][0], &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_align(k, LV_ALIGN_TOP_LEFT, 6, 6 + i * 22);
        /* 值含中文/×/·（"双核 LX7 · 240 MHz"）→ 必须用 cn 字体
         * （montserrat 无 CJK，曾整行豆腐） */
        lv_obj_t *v = ui_label(card2, rows[i][1], &lv_font_cn_14, UI_TEXT);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -6, 6 + i * 22);
    }

    lv_obj_t *note = ui_label(scr,
        "电源为模拟值（真机 AXP2101 采集）；内存/Flash 动态指标 Phase 6 接入",
        &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* ---------------- 页面分发 ---------------- */

void ui_settings_build_page(lv_obj_t *scr)
{
    lv_obj_set_style_pad_all(scr, 16, 0);

    /* 子页切换：杀网络页轮询（timer 持有的 label 属于被 clean 的旧页） */
    if (g_net_timer) {
        lv_timer_delete(g_net_timer);
        g_net_timer = NULL;
        g_net_ssid_lbl = g_net_ip_lbl = g_net_bh_val_lbl = g_net_admin_val_lbl = NULL;
    }

    switch (g_page) {
    case SP_DISPLAY:
        subpage_gesture_add(scr);
        page_display(scr);
        break;
    case SP_NETWORK:
        subpage_gesture_add(scr);
        page_network(scr);
        break;
    case SP_SERVICE:
        subpage_gesture_add(scr);
        page_service(scr);
        break;
    case SP_DEVICE:
        subpage_gesture_add(scr);
        page_device(scr);
        break;
    case SP_HOME:
    default: {
        /* 首页：左上返回键（与其他子页统一，回 Launcher）+ 居中标题 */
        lv_obj_t *t = ui_label(scr, "系统设置", &lv_font_cn_20, UI_TEXT);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

        ui_gesture_back_add(scr); /* 上滑退出设置 App */

        struct { int page; const char *t; const char *d; const char *s; uint32_t c; } items[] = {
            { SP_DISPLAY, "显示与声音", "音量、亮度、息屏",   LV_SYMBOL_SETTINGS, 0x7C5CFF },
            { SP_NETWORK, "网络与连接", "Wi-Fi、配网、二维码", LV_SYMBOL_WIFI,     0x2EC4B6 },
            { SP_SERVICE, "服务管理",   "订阅刷新、后台服务",  LV_SYMBOL_REFRESH,  0xFF8A3D },
            { SP_DEVICE,  "设备信息",   "版本、内存、硬件",    LV_SYMBOL_LIST,     0x3A6FF7 },
        };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *card = entry_card(scr, items[i].page, items[i].t,
                                        items[i].d, items[i].s, items[i].c);
            lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 86 + i * 88);
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

    /* 验收钩子：GADGET_DEMO_SETTINGS_PAGE=display|network|service|device
     * 直达子页（与 GADGET_DEMO_APP 同族，仅演示/截图用） */
    const char *sp = getenv("GADGET_DEMO_SETTINGS_PAGE");
    if (sp && sp[0]) {
        if (strcmp(sp, "display") == 0) g_page = SP_DISPLAY;
        else if (strcmp(sp, "network") == 0) g_page = SP_NETWORK;
        else if (strcmp(sp, "service") == 0) g_page = SP_SERVICE;
        else if (strcmp(sp, "device") == 0) g_page = SP_DEVICE;
    }

    ui_settings_build_page(scr);
}

void ui_settings_on_exit(void)
{
    g_page = SP_HOME;
    g_root = NULL;
}
