/* ui_settings — 分类式设置（PRODUCT_SPEC §4，P0）：
 * 设置首页（四入口卡 + 固件状态行）→ 子页（显示与声音 / 网络与连接 /
 * 服务管理 / 设备信息）。子页 back 回首页；首页 back 回 Launcher。
 * 设置存 data_source 内存态；真机走 device_config NVS（Phase 6）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_manager.h"
#include "data_source.h"
#include "http_client.h" /* http_probe：网络诊断 */
#include "ui_worker.h"   /* ui_worker_start：诊断探测 worker */
#include "ui.h"
#include "ui_gesture.h"
#include "ui_launcher.h"
#include "ui_theme.h"
#include "ui_wifi_panel.h"

#ifdef ESP_PLATFORM
#include "bsp/esp-bsp.h"
/* 与 ui_statusbar 同款：LVGL 回调内已在显示锁里，QSPI 亮度事务安全 */
#define BRIGHT_APPLY(v) bsp_display_brightness_set(v)
#else
#define BRIGHT_APPLY(v) ((void)0)
#endif

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
    if (which[0] == 'b') {
        ds_config_update(v, -1, 0);
        BRIGHT_APPLY(v); /* 此前只改状态不应用——真机滑条是死的，顺手修 */
    }
    else ds_config_update(0, v, 0);
    ds_config_persist(); /* 防抖回写 Bridge（亮度/音量跨重启持久化） */
}

static void timeout_changed_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    static const int vals[] = { 15, 30, 60, 120, 300 };
    int idx = (int)lv_dropdown_get_selected(dd);
    if (idx >= 0 && idx < 5) {
        ds_config_update(0, 0, vals[idx]);
        ds_config_persist();
    }
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

/* ---- 网络诊断（门户检测 + 二维码，2026-11-17）----
 * 探测当前 Bridge 地址：200=通；3xx=门户劫持（弹认证页二维码——
 * 注意多数门户按 MAC 放行，手机扫码认证的是手机自己，设备可能仍
 * 不通，弹窗文案如实说明）；-4=阻断型网络（公司访客网常见：Wi-Fi
 * 能连 DNS 能解析，但出网 TCP 被防火墙丢弃——扫码也无认证页可扫）。
 * worker 探测（勿阻塞 LVGL 线程，踩坑 #11）；结果弹窗挂 lv_layer_top
 * 自成一体，不依赖子页存活。 */
static char s_probe_loc[384];
static int s_probe_status;

static void probe_result_async(void *arg)
{
    (void)arg;
    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mask, 480, 480);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mask, qr_close_cb, LV_EVENT_CLICKED, mask);

    lv_obj_t *title = ui_label(mask, "网络诊断", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *body = NULL;
    int body_y = 80;
    if (s_probe_status == 200) {
        body = ui_label(mask, "✓ 网络正常\nBridge 连接畅通", &lv_font_cn_16, UI_OK);
    } else if (s_probe_status >= 300 && s_probe_status < 400 && s_probe_loc[0]) {
        body = ui_label(mask, "检测到门户认证网络\n手机连同一 Wi-Fi 扫码打开认证页\n"
                             "注意：多数门户按设备放行\n手机认证后本设备可能仍不通",
                        &lv_font_cn_14, UI_WARN);
        body_y = 56;
        lv_obj_t *qr = lv_qrcode_create(mask);
        lv_qrcode_update(qr, s_probe_loc, (uint32_t)strlen(s_probe_loc));
        lv_obj_set_size(qr, 260, 260);
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 128);
    } else if (s_probe_status == -4) {
        body = ui_label(mask, "网络被阻断（非门户）\nWi-Fi 已连接但出网被拦截\n"
                             "公司访客网常见——无认证页可扫\n换网络或用手机热点共享",
                        &lv_font_cn_14, UI_WARN);
        body_y = 56;
    } else if (s_probe_status == -2) {
        body = ui_label(mask, "DNS 解析失败\n检查 Bridge 地址拼写\n或当前网络无 DNS 服务",
                        &lv_font_cn_14, UI_WARN);
        body_y = 56;
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf), "探测失败：status=%d", s_probe_status);
        body = ui_label(mask, buf, &lv_font_cn_14, UI_WARN);
        body_y = 56;
    }
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, body_y);

    lv_obj_t *hint = ui_label(mask, "点击任意处关闭", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void *probe_worker(void *arg)
{
    (void)arg;
    const char *base = ds_bridge_active_base(); /* 当前激活地址（mock 为空） */
    if (!base || !base[0]) base = "http://127.0.0.1:8081"; /* mock 兜底 */
    char url[224];
    snprintf(url, sizeof(url), "%s/api/v1/subscriptions", base);
    s_probe_loc[0] = '\0';
    s_probe_status = http_probe(url, s_probe_loc, sizeof(s_probe_loc), 8000);
    ui_lock();
    lv_async_call(probe_result_async, NULL);
    ui_unlock();
    return NULL;
}

static void probe_open_cb(lv_event_t *e)
{
    (void)e;
    if (ui_worker_start(probe_worker, NULL, 12 * 1024) != 0) {
        /* ENOMEM 静默失败是踩坑 #13 的教训：必须给用户反馈 */
        lv_obj_t *mask = lv_obj_create(lv_layer_top());
        lv_obj_set_size(mask, 480, 480);
        lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(mask, lv_color_hex(0x0B1020), 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(mask, 0, 0);
        lv_obj_add_event_cb(mask, qr_close_cb, LV_EVENT_CLICKED, mask);
        lv_obj_t *t = ui_label(mask, "内存紧张，请稍后重试", &lv_font_cn_16, UI_WARN);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 200);
    }
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

    /* 行 5：网络诊断（门户检测+二维码；公司访客网/酒店 Wi-Fi 排障） */
    lv_obj_t *r5 = lv_button_create(scr);
    lv_obj_set_size(r5, LV_PCT(100) - 8, 48);
    lv_obj_align(r5, LV_ALIGN_TOP_MID, 0, 356);
    ui_style_card(r5);
    lv_obj_set_style_bg_color(r5, UI_CARD_HI, 0);
    lv_obj_add_event_cb(r5, probe_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t5 = ui_label(r5, "网络诊断", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(t5, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *h5 = ui_label(r5, LV_SYMBOL_SETTINGS " 门户检测", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(h5, LV_ALIGN_RIGHT_MID, -10, 0);

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

/* ---------------- 页面：系统状态（仪表盘，设计稿 霓虹系统状态仪表盘） ----------------
 * SRAM/PSRAM/Flash/TF/CPU 五卡 + 底部运行时间/设备信息条。采集走 worker
 * （FATFS 扫描/分区遍历阻塞，坑 #11），页面代际防悬垂，30s 自动刷新。 */

static lv_timer_t *g_devs_timer;
static unsigned g_devs_gen; /* 页面代际：切页/重建即失效在途采集 */
static volatile bool g_devs_busy;
static bool g_devs_valid;

/* 动态控件引用（页面销毁即作废，build_page 切页时置 NULL） */
static lv_obj_t *g_devs_time_lbl;                       /* 刷新·HH:MM:SS */
static lv_obj_t *g_devs_pct[4];                         /* 0=SRAM 1=PSRAM 2=Flash 3=TF */
static lv_obj_t *g_devs_data[4], *g_devs_note[4], *g_devs_bar[4];
static lv_obj_t *g_devs_temp_lbl, *g_devs_load_lbl, *g_devs_up_lbl;
static lv_obj_t *g_devs_chip_lbl, *g_devs_fw_lbl;

static void fmt_bytes(char *buf, int cap, uint64_t b)
{
    if (b < 1024ull * 1024) snprintf(buf, cap, "%u KB", (unsigned)((b + 1023) / 1024));
    else if (b < 10ull * 1024 * 1024 * 1024)
        snprintf(buf, cap, "%.1f MB", b / 1048576.0);
    else snprintf(buf, cap, "%.1f GB", b / 1073741824.0);
}

static int pct_of(uint64_t used, uint64_t total)
{
    if (total == 0 || used > total) return 0;
    return (int)(used * 100 / total);
}

/* 卡片备注用短单位（K/M/G）——cn_12 下 "剩余 226 KB · 最低 180 KB"
 * 超 142px 卡宽被截断，实测要 ≤17 半角位 */
static void fmt_bytes_short(char *buf, int cap, uint64_t b)
{
    if (b < 1024ull * 1024) snprintf(buf, cap, "%uK", (unsigned)((b + 1023) / 1024));
    else if (b < 10ull * 1024 * 1024 * 1024)
        snprintf(buf, cap, "%.1fM", b / 1048576.0);
    else snprintf(buf, cap, "%.1fG", b / 1073741824.0);
}

static void fmt_uptime(char *buf, int cap, uint32_t s)
{
    uint32_t d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d > 0) snprintf(buf, cap, "%u 天 %u 小时", (unsigned)d, (unsigned)h);
    else if (h > 0) snprintf(buf, cap, "%u 小时 %u 分", (unsigned)h, (unsigned)m);
    else snprintf(buf, cap, "%u 分钟", (unsigned)m);
}

/* 采集结果落点（worker 线程写，LVGL 线程读） */
static ds_devstats_t g_devs_snap;

/* 把最近一次采集结果写进控件（LVGL 线程；控件可能已 NULL） */
static void devs_apply(void)
{
    if (!g_devs_valid) return;
    const ds_devstats_t *s = &g_devs_snap;

    char a[16], b[16], data[48], note[96], pct[8];
    /* SRAM：已用% / 已用+总量 / 剩余·历史最低 */
    uint32_t used = s->sram_total - s->sram_free;
    snprintf(pct, sizeof(pct), "%d%%", pct_of(used, s->sram_total));
    fmt_bytes(a, sizeof(a), used);
    fmt_bytes(b, sizeof(b), s->sram_total);
    snprintf(data, sizeof(data), "%s / %s", a, b);
    fmt_bytes_short(a, sizeof(a), s->sram_free);
    fmt_bytes_short(b, sizeof(b), s->sram_min);
    snprintf(note, sizeof(note), "剩 %s · 最低 %s", a, b);
    lv_label_set_text(g_devs_pct[0], pct);
    lv_label_set_text(g_devs_data[0], data);
    lv_label_set_text(g_devs_note[0], note);
    lv_bar_set_value(g_devs_bar[0], pct_of(used, s->sram_total), LV_ANIM_OFF);

    /* PSRAM：堆池口径（实配 8MB，一部分被指令/常量窗口占用进不了堆） */
    uint32_t pused = s->psram_total - s->psram_free;
    snprintf(pct, sizeof(pct), "%d%%", pct_of(pused, s->psram_total));
    fmt_bytes(a, sizeof(a), pused);
    fmt_bytes(b, sizeof(b), s->psram_total);
    snprintf(data, sizeof(data), "%s / %s", a, b);
    fmt_bytes_short(a, sizeof(a), s->psram_free);
    snprintf(note, sizeof(note), "剩 %s · 实配 8M", a);
    lv_label_set_text(g_devs_pct[1], pct);
    lv_label_set_text(g_devs_data[1], data);
    lv_label_set_text(g_devs_note[1], note);
    lv_bar_set_value(g_devs_bar[1], pct_of(pused, s->psram_total), LV_ANIM_OFF);

    /* Flash：程序分区 + 内置 storage 分区合计；未挂载只计容量 */
    uint64_t fused = (uint64_t)s->flash_app_used + s->flash_fs_used;
    uint64_t ftotal = (uint64_t)s->flash_app_total + s->flash_fs_total;
    snprintf(pct, sizeof(pct), "%d%%", pct_of(fused, ftotal));
    fmt_bytes(a, sizeof(a), fused);
    fmt_bytes(b, sizeof(b), ftotal);
    snprintf(data, sizeof(data), "%s / %s", a, b);
    fmt_bytes_short(a, sizeof(a), s->flash_app_used);
    fmt_bytes_short(b, sizeof(b), s->flash_fs_total);
    snprintf(note, sizeof(note), "程序 %s · 内置 %s", a, b);
    lv_label_set_text(g_devs_pct[2], pct);
    lv_label_set_text(g_devs_data[2], data);
    lv_label_set_text(g_devs_note[2], note);
    lv_bar_set_value(g_devs_bar[2], pct_of(fused, ftotal), LV_ANIM_OFF);

    /* TF 卡 */
    if (s->tf_mounted && s->tf_total > 0) {
        snprintf(pct, sizeof(pct), "%d%%", pct_of(s->tf_used, s->tf_total));
        fmt_bytes(a, sizeof(a), s->tf_used);
        fmt_bytes(b, sizeof(b), s->tf_total);
        snprintf(data, sizeof(data), "%s / %s", a, b);
        fmt_bytes_short(a, sizeof(a), s->tf_total - s->tf_used);
        snprintf(note, sizeof(note), "剩 %s · 已挂载", a);
    } else {
        snprintf(pct, sizeof(pct), "--");
        snprintf(data, sizeof(data), "未插卡");
        snprintf(note, sizeof(note), "文件 App 将静默降级");
    }
    lv_label_set_text(g_devs_pct[3], pct);
    lv_label_set_text(g_devs_data[3], data);
    lv_label_set_text(g_devs_note[3], note);
    lv_bar_set_value(g_devs_bar[3], s->tf_mounted ? pct_of(s->tf_used, s->tf_total) : 0, LV_ANIM_OFF);

    /* CPU 温度/负载（℃ 字形不在字库，用 °C——° 在生成字符集里） */
    if (s->cpu_temp >= 0) snprintf(note, sizeof(note), "%.1f°C", (double)s->cpu_temp);
    else snprintf(note, sizeof(note), "--");
    lv_label_set_text(g_devs_temp_lbl, note);
    snprintf(note, sizeof(note), "%d%%", s->cpu_load >= 0 ? s->cpu_load : 0);
    lv_label_set_text(g_devs_load_lbl, s->cpu_load >= 0 ? note : "--");

    /* 底部：运行时间 + 设备信息 */
    fmt_uptime(note, sizeof(note), s->uptime_s);
    lv_label_set_text(g_devs_up_lbl, note);
    lv_label_set_text(g_devs_chip_lbl, s->chip);
    snprintf(note, sizeof(note), "固件版本 %s", s->fw_ver);
    lv_label_set_text(g_devs_fw_lbl, note);

    /* 刷新时间 */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tb[24];
    snprintf(tb, sizeof(tb), LV_SYMBOL_REFRESH " %02d:%02d",
             tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(g_devs_time_lbl, tb);
}

static void devs_apply_async(void *arg)
{
    (void)arg;
    if (g_page == SP_DEVICE && g_root && g_devs_time_lbl) devs_apply();
}

static void *devs_worker(void *arg)
{
    (void)arg;
    unsigned gen = g_devs_gen;
    ds_devstats_fetch(&g_devs_snap);
    g_devs_valid = true;
    g_devs_busy = false;
    if (gen == g_devs_gen) lv_async_call(devs_apply_async, NULL);
    return NULL;
}

static void devs_fetch_start(void)
{
    if (g_devs_busy) return;
    g_devs_busy = true;
#ifdef ESP_PLATFORM
    /* 真机：投常驻静态栈语音任务（坑 #13/#15 模式，与订阅页同款）。
     * pthread 走不通——2026-11-17 实测设置页打开后 largest_int 掉到
     * 10.2KB，16KB/12KB 两档全灭，采集静默不跑、页面全是 "--"。
     * busy（语音占用中）跳过本轮，30s 后自动重试。 */
    int rc = ui_bg_task_post(devs_worker, NULL);
    if (rc != 0) {
        printf("devs fetch: bg task busy/skip rc=%d\n", rc);
        g_devs_busy = false;
    }
#else
    if (ui_worker_start(devs_worker, NULL, 16 * 1024) != 0) g_devs_busy = false;
#endif
}

static void devs_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_page == SP_DEVICE && g_root) devs_fetch_start();
}

static void devs_refresh_cb(lv_event_t *e)
{
    (void)e;
    devs_fetch_start();
}

/* 指标卡（SRAM/PSRAM/Flash/TF 共用骨架） */
static lv_obj_t *devs_metric_card(lv_obj_t *parent, int w, int h,
                                  const char *sym, uint32_t sym_color,
                                  const char *title, uint32_t val_color,
                                  uint32_t grad_to, int idx)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    ui_style_card(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = ui_label(card, sym, &lv_font_montserrat_14,
                            lv_color_hex(sym_color));
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *t = ui_label(card, title, &lv_font_cn_14, UI_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 22, 0);

    g_devs_pct[idx] = ui_label(card, "--", &lv_font_montserrat_28,
                               lv_color_hex(val_color));
    lv_obj_align(g_devs_pct[idx], LV_ALIGN_TOP_LEFT, 2, 20);

    g_devs_data[idx] = ui_label(card, "--", &lv_font_cn_12,
                                UI_TEXT);
    lv_obj_align(g_devs_data[idx], LV_ALIGN_TOP_LEFT, 2, 56);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, w - 20, 8);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 2, 76);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A2A45), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(val_color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(grad_to), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    g_devs_bar[idx] = bar;

    g_devs_note[idx] = ui_label(card, "--", &lv_font_cn_12,
                                UI_TEXT);
    lv_obj_align(g_devs_note[idx], LV_ALIGN_TOP_LEFT, 2, 92);
    return card;
}

static void page_device(lv_obj_t *scr)
{
    page_title(scr, "系统状态");

    /* 采集代际 +1：上一轮在途结果作废 */
    g_devs_gen++;
    g_devs_valid = false;

    /* 行 1：SRAM / PSRAM / Flash（142×138 ×3） */
    lv_obj_t *c0 = devs_metric_card(scr, 142, 138, LV_SYMBOL_BARS, 0x38BDF8,
                                    "SRAM", 0x38BDF8, 0x3B82F6, 0);
    lv_obj_align(c0, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_t *c1 = devs_metric_card(scr, 142, 138, LV_SYMBOL_DRIVE, 0xC084FC,
                                    "PSRAM", 0xC084FC, 0xA855F7, 1);
    lv_obj_align(c1, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *c2 = devs_metric_card(scr, 142, 138, LV_SYMBOL_SAVE, 0x4ADE80,
                                    "Flash", 0x4ADE80, 0x22C55E, 2);
    lv_obj_align(c2, LV_ALIGN_TOP_RIGHT, 0, 40);

    /* 行 2：TF 卡（左 209）+ CPU 温度/负载（右 229，双栏），加高防底注裁切 */
    lv_obj_t *tf = devs_metric_card(scr, 209, 118, LV_SYMBOL_SD_CARD, 0x34D399,
                                    "TF 卡", 0x34D399, 0x2DD4BF, 3);
    lv_obj_align(tf, LV_ALIGN_TOP_LEFT, 0, 186);
    /* TF 卡紧凑版：大数字与数据同行、条与备注上移（骨架 helper 后微调） */
    lv_obj_align(g_devs_pct[3], LV_ALIGN_TOP_LEFT, 2, 22);
    lv_obj_align(g_devs_data[3], LV_ALIGN_TOP_RIGHT, -2, 32);
    lv_obj_align(g_devs_bar[3], LV_ALIGN_TOP_LEFT, 2, 72);
    lv_obj_align(g_devs_note[3], LV_ALIGN_TOP_LEFT, 2, 94);

    lv_obj_t *cpu = lv_obj_create(scr);
    lv_obj_set_size(cpu, 229, 118);
    lv_obj_align(cpu, LV_ALIGN_TOP_RIGHT, 0, 186);
    ui_style_card(cpu);
    lv_obj_clear_flag(cpu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *div = lv_obj_create(cpu);
    lv_obj_set_size(div, 1, 90);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x2A2A55), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);

    lv_obj_t *ti = ui_label(cpu, LV_SYMBOL_TINT, &lv_font_montserrat_14,
                            lv_color_hex(0xF59E0B));
    lv_obj_align(ti, LV_ALIGN_TOP_LEFT, 12, 6);
    lv_obj_t *tt = ui_label(cpu, "CPU 温度", &lv_font_cn_12, UI_TEXT);
    lv_obj_align(tt, LV_ALIGN_TOP_LEFT, 32, 4);
    g_devs_temp_lbl = ui_label(cpu, "--", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(g_devs_temp_lbl, LV_ALIGN_TOP_LEFT, 12, 32);
    lv_obj_t *tn = ui_label(cpu, "芯片内部温度", &lv_font_cn_12,
                            UI_TEXT);
    lv_obj_align(tn, LV_ALIGN_TOP_LEFT, 12, 72);

    lv_obj_t *li = ui_label(cpu, LV_SYMBOL_LOOP, &lv_font_montserrat_14,
                            lv_color_hex(0x38BDF8));
    lv_obj_align(li, LV_ALIGN_TOP_LEFT, 128, 6);
    lv_obj_t *lt = ui_label(cpu, "CPU 负载", &lv_font_cn_12, UI_TEXT);
    lv_obj_align(lt, LV_ALIGN_TOP_LEFT, 148, 4);
    g_devs_load_lbl = ui_label(cpu, "--", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(g_devs_load_lbl, LV_ALIGN_TOP_LEFT, 128, 32);
    lv_obj_t *ln = ui_label(cpu, "双核平均值", &lv_font_cn_12,
                            UI_TEXT);
    lv_obj_align(ln, LV_ALIGN_TOP_LEFT, 128, 72);

    /* 行 3：运行时间 | 设备信息（加高：72→90，"自上次开机"曾被裁切） */
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_set_size(bot, 448, 90);
    lv_obj_align(bot, LV_ALIGN_TOP_MID, 0, 312);
    ui_style_card(bot);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *div2 = lv_obj_create(bot);
    lv_obj_set_size(div2, 1, 62);
    lv_obj_align(div2, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_bg_color(div2, lv_color_hex(0x2A2A55), 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div2, 0, 0);

    lv_obj_t *ul = ui_label(bot, "系统运行时间", &lv_font_cn_12,
                            UI_TEXT);
    lv_obj_align(ul, LV_ALIGN_TOP_LEFT, 14, 10);
    g_devs_up_lbl = ui_label(bot, "--", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(g_devs_up_lbl, LV_ALIGN_TOP_LEFT, 14, 32);
    lv_obj_t *un = ui_label(bot, "自上次开机", &lv_font_cn_12,
                            UI_TEXT);
    lv_obj_align(un, LV_ALIGN_TOP_LEFT, 14, 62);

    lv_obj_t *cl = ui_label(bot, "设备信息", &lv_font_cn_12,
                            UI_TEXT);
    lv_obj_align(cl, LV_ALIGN_TOP_LEFT, 238, 10);
    g_devs_chip_lbl = ui_label(bot, "--", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(g_devs_chip_lbl, LV_ALIGN_TOP_LEFT, 238, 32);
    g_devs_fw_lbl = ui_label(bot, "--", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(g_devs_fw_lbl, LV_ALIGN_TOP_LEFT, 238, 56);

    /* 底部：刷新胶囊（可点，采集后显示时间） */
    lv_obj_t *pill = lv_obj_create(scr);
    lv_obj_set_size(pill, 150, 36);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 410);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x1A1A3E), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0x2A2A55), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_radius(pill, 18, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pill, devs_refresh_cb, LV_EVENT_CLICKED, NULL);
    g_devs_time_lbl = ui_label(pill, LV_SYMBOL_REFRESH " 刷新",
                               &lv_font_cn_14, lv_color_hex(0x22D3EE));
    lv_obj_center(g_devs_time_lbl);

    /* 首采 + 30s 自动刷新（页面关闭/切子页即杀） */
    devs_fetch_start();
    g_devs_timer = lv_timer_create(devs_timer_cb, 30000, NULL);
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
    /* 同理：系统状态页轮询与在途采集（代际 +1 令其结果作废） */
    if (g_devs_timer) {
        lv_timer_delete(g_devs_timer);
        g_devs_timer = NULL;
    }
    g_devs_gen++;
    g_devs_time_lbl = NULL;
    g_devs_temp_lbl = g_devs_load_lbl = g_devs_up_lbl = NULL;
    g_devs_chip_lbl = g_devs_fw_lbl = NULL;
    for (int i = 0; i < 4; i++) {
        g_devs_pct[i] = g_devs_data[i] = g_devs_note[i] = g_devs_bar[i] = NULL;
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
            { SP_DEVICE,  "系统状态",   "内存、存储、CPU",     LV_SYMBOL_LIST,     0x3A6FF7 },
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
    if (g_devs_timer) {
        lv_timer_delete(g_devs_timer);
        g_devs_timer = NULL;
    }
    g_devs_gen++; /* 在途采集结果作废 */
}
