/* ui_wifi_panel.c — 运行期配网面板（Wi-Fi 扫描/密码 + Bridge 地址）。
 *
 * 线程模型（照 ui_conversation 的 bot picker，真机验证过的范式）：
 *   - 阻塞操作（ds_wifi_scan ~3s / ds_wifi_connect ≤15s）跑在 16KB
 *     pthread 后台线程；UI 只在 LVGL 线程动（点击回调同步建、worker
 *     完成后 lv_async_call 更新）。
 *   - 真机注意：建对象绝不在 worker 线程里做（跨线程建 UI 曾把显示
 *     管线卡死）；持锁只做 label 文本这类轻更新。
 * 键盘：lv_keyboard + lv_textarea（双端 LV_USE_KEYBOARD/TEXTAREA 已开；
 * 按键字符 ASCII，montserrat 渲染；中文提示用 font_cn_14）。 */
#include "ui_wifi_panel.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_source.h"
#include "ui.h"
#include "ui_theme.h"

#define WIFI_AP_MAX 12
#define PANEL_BG 0x0B1020

/* ---- 面板共享状态（LVGL 线程与 worker 之间只经 lv_async_call 传递） ---- */
static lv_obj_t *g_mask;        /* 全屏遮罩（两视图共用父容器） */
static lv_obj_t *g_view;        /* 当前视图容器（列表 或 密码/地址输入） */
static char s_cur_ssid[33];     /* 当前已连接 SSID（列表 ✓ 标记） */
static char s_pick_ssid[33];    /* 密码视图目标 SSID */
static char s_pick_pass[64];    /* 密码（点击回调在 LVGL 线程拷出，worker 只读这里） */
static ds_wifi_ap_t s_aps[WIFI_AP_MAX];
static int s_ap_n;
static volatile bool s_worker_busy; /* 扫描/连接进行中（防重入） */

/* ---- 工具 ---- */

static void panel_thread_start(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 * 1024); /* bot picker 同款：HTTP/扫描 4KB+ */
    pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    pthread_detach(t);
}

static lv_obj_t *panel_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static void mask_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_mask) {
        lv_obj_delete(g_mask);
        g_mask = NULL;
        g_view = NULL;
    }
    s_worker_busy = false;
}

static lv_obj_t *mask_create(const char *title)
{
    if (g_mask) mask_close_cb(NULL); /* 防重入 */
    g_mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_mask, 480, 480);
    lv_obj_align(g_mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(g_mask, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_opa(g_mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_mask, 0, 0);
    lv_obj_set_style_pad_all(g_mask, 0, 0);
    lv_obj_clear_flag(g_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ttl = panel_label(g_mask, title, &lv_font_cn_16, UI_TEXT);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 14);
    return g_mask;
}

/* ================= Wi-Fi 列表视图 ================= */

static void wifi_pass_view_build(const char *ssid); /* 前向 */

static void wifi_connect_done_async(void *arg)
{
    int ok = (int)(intptr_t)arg;
    if (!g_mask) return; /* 用户已关面板 */
    lv_obj_t *lbl = lv_obj_get_child(g_view, 0); /* 状态行在视图顶部 */
    if (!lbl) return;
    if (ok) {
        mask_close_cb(NULL);
        return; /* 成功：关面板（网络页状态卡由其轮询刷新） */
    }
    lv_label_set_text(lbl, "连接失败：密码可能不对，重输或返回");
    lv_obj_set_style_text_color(lbl, UI_ERR, 0);
}

static void *wifi_connect_worker(void *arg)
{
    (void)arg;
    int rc = ds_wifi_connect(s_pick_ssid, s_pick_pass);
    ui_lock();
    lv_async_call(wifi_connect_done_async, (void *)(intptr_t)(rc == 0));
    ui_unlock();
    return NULL;
}

static void wifi_connect_cb(lv_event_t *e)
{
    (void)e;
    if (s_worker_busy) return;
    s_worker_busy = true;
    /* 密码在 LVGL 线程拷出（worker 不碰 widget） */
    lv_obj_t *ta = lv_obj_get_child(g_view, 1);
    strlcpy(s_pick_pass, lv_textarea_get_text(ta), sizeof(s_pick_pass));
    lv_obj_t *lbl = lv_obj_get_child(g_view, 0);
    lv_label_set_text(lbl, "连接中…（最长 15 秒）");
    lv_obj_set_style_text_color(lbl, UI_TEXT_DIM, 0);
    panel_thread_start(wifi_connect_worker, NULL);
}

static void wifi_item_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    strlcpy(s_pick_ssid, s_aps[idx].ssid, sizeof(s_pick_ssid));
    wifi_pass_view_build(s_pick_ssid);
}

static void *wifi_scan_worker(void *arg)
{
    (void)arg;
    int n = 0;
    int rc = ds_wifi_scan(s_aps, WIFI_AP_MAX, &n);
    s_ap_n = (rc == 0) ? n : 0;
    ui_lock();
    lv_async_call((lv_async_cb_t)arg, NULL);
    ui_unlock();
    return NULL;
}

static void wifi_list_fill(void); /* 前向 */

static void wifi_scan_done_async(void *arg)
{
    (void)arg;
    s_worker_busy = false;
    if (!g_mask) return;
    wifi_list_fill();
    /* 回归钩子：扫描完成后直接进密码视图（截帧验证键盘/textarea 渲染） */
    if (getenv("GADGET_DEMO_WIFI_PASS") && s_ap_n > 0) {
        strlcpy(s_pick_ssid, s_aps[0].ssid, sizeof(s_pick_ssid));
        wifi_pass_view_build(s_pick_ssid);
    }
}

/* 重新扫描按钮 */
static void wifi_rescan_cb(lv_event_t *e)
{
    (void)e;
    if (s_worker_busy) return;
    if (g_view) {
        lv_obj_delete(g_view);
        g_view = NULL;
    }
    g_view = lv_obj_create(g_mask);
    lv_obj_set_size(g_view, 480, 480);
    lv_obj_set_style_bg_opa(g_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_view, 0, 0);
    lv_obj_set_style_pad_all(g_view, 0, 0);
    lv_obj_clear_flag(g_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *st = panel_label(g_view, "扫描中…（约 3 秒）", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 56);
    s_worker_busy = true;
    panel_thread_start(wifi_scan_worker, (void *)wifi_scan_done_async);
}

/* 列表视图：状态行 + AP 列表 + 底部按钮 */
static void wifi_list_fill(void)
{
    if (g_view) {
        lv_obj_delete(g_view);
        g_view = NULL;
    }
    ds_wifi_status(s_cur_ssid, sizeof(s_cur_ssid), NULL, 0);

    g_view = lv_obj_create(g_mask);
    lv_obj_set_size(g_view, 460, 350);
    lv_obj_align(g_view, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_opa(g_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_view, 0, 0);
    lv_obj_set_style_pad_all(g_view, 4, 0);
    lv_obj_set_flex_flow(g_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_view, 6, 0);

    if (s_ap_n == 0) {
        lv_obj_t *em = panel_label(g_view, "没扫到网络（点\"重新扫描\"重试）",
                                   &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_set_style_pad_top(em, 40, 0);
    }
    for (int i = 0; i < s_ap_n; i++) {
        char row[80];
        snprintf(row, sizeof(row), "%.32s  %d%s", s_aps[i].ssid, s_aps[i].rssi,
                 s_aps[i].secure ? " " LV_SYMBOL_EYE_CLOSE : "");
        lv_obj_t *btn = lv_button_create(g_view);
        lv_obj_set_size(btn, LV_PCT(100), 46);
        lv_obj_add_event_cb(btn, wifi_item_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *l = panel_label(btn, row, &lv_font_cn_14, UI_TEXT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        if (strcmp(s_aps[i].ssid, s_cur_ssid) == 0) {
            lv_obj_t *ok = panel_label(btn, LV_SYMBOL_OK, &lv_font_montserrat_14,
                                       UI_ACCENT);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }

    /* 底部按钮：重新扫描 / 关闭 */
    lv_obj_t *rescan = lv_button_create(g_mask);
    lv_obj_set_size(rescan, 190, 46);
    lv_obj_align(rescan, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    lv_obj_add_event_cb(rescan, wifi_rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = panel_label(rescan, "重新扫描", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(rl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *close = lv_button_create(g_mask);
    lv_obj_set_size(close, 190, 46);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
    lv_obj_add_event_cb(close, mask_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = panel_label(close, "关闭", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(cl, LV_ALIGN_CENTER, 0, 0);
}

void ui_wifi_panel_open(void)
{
    if (g_mask) mask_close_cb(NULL);
    mask_create("选择 Wi-Fi");
    wifi_rescan_cb(NULL); /* 复用：建"扫描中"视图并启动 worker */
}

/* ================= 密码输入视图 ================= */

static void wifi_pass_view_build(const char *ssid)
{
    if (g_view) {
        lv_obj_delete(g_view);
        g_view = NULL;
    }
    g_view = lv_obj_create(g_mask);
    lv_obj_set_size(g_view, 480, 480);
    lv_obj_set_style_bg_opa(g_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_view, 0, 0);
    lv_obj_set_style_pad_all(g_view, 0, 0);
    lv_obj_clear_flag(g_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *st = panel_label(g_view, "输入 Wi-Fi 密码（开放网络留空）",
                               &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 20, 46);

    lv_obj_t *ta = lv_textarea_create(g_view);
    lv_obj_set_size(ta, 440, 54);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 74);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, 63);
    lv_textarea_set_placeholder_text(ta, ssid); /* 目标网络名放占位 */

    lv_obj_t *conn = lv_button_create(g_view);
    lv_obj_set_size(conn, 120, 44);
    lv_obj_align(conn, LV_ALIGN_TOP_LEFT, 20, 140);
    lv_obj_add_event_cb(conn, wifi_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl1 = panel_label(conn, "连接", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(cl1, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *back = lv_button_create(g_view);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 152, 140);
    lv_obj_add_event_cb(back, wifi_rescan_cb, LV_EVENT_CLICKED, NULL); /* 回列表=重扫 */
    lv_obj_t *cl2 = panel_label(back, "返回列表", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(cl2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *kb = lv_keyboard_create(g_view);
    lv_obj_set_size(kb, 480, 240);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
}

/* ================= Bridge 地址（地址簿：多地址保存/切换/添加） ================= */

#define BH_PANEL_MAX 8

static char s_bh_rows[BH_PANEL_MAX][160]; /* 地址簿快照（LVGL 线程用） */
static int s_bh_n;
static char s_bh_cur[160];               /* 当前生效地址 */

static void bridge_list_view_build(void); /* 前向 */
static void bridge_add_view_build(void);  /* 前向 */

static void bridge_list_view_cb(lv_event_t *e)
{
    (void)e;
    bridge_list_view_build();
}
static void bridge_add_view_cb(lv_event_t *e)
{
    (void)e;
    bridge_add_view_build();
}

static void bridge_list_refresh(void)
{
    char raw[BH_PANEL_MAX * 161];
    s_bh_n = 0;
    ds_bridge_host_get(s_bh_cur, sizeof(s_bh_cur));
    if (ds_bridge_host_list(raw, sizeof(raw)) == 0) {
        char *p = raw;
        while (*p && s_bh_n < BH_PANEL_MAX) {
            char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len > 0 && len < sizeof(s_bh_rows[0])) {
                memcpy(s_bh_rows[s_bh_n], p, len);
                s_bh_rows[s_bh_n][len] = '\0';
                s_bh_n++;
            }
            if (!e) break;
            p = e + 1;
        }
    }
}

/* 切换（点地址行）：保存+rebind，关面板（网络页状态行随之刷新） */
static void bridge_switch_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_bh_n) return;
    if (strcmp(s_bh_rows[idx], s_bh_cur) != 0) {
        ds_bridge_host_save(s_bh_rows[idx]);
        ds_bridge_rebind(s_bh_rows[idx]); /* 热更新数据源全部 URL */
    }
    mask_close_cb(NULL);
}

/* 删除（非当前地址）：从地址簿移除后重建列表 */
static void bridge_del_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_bh_n) return;
    if (ds_bridge_host_remove(s_bh_rows[idx]) == 0) {
        bridge_list_refresh();
        bridge_list_view_build();
    }
}

/* 添加视图：保存 → 入簿并激活 */
static void bridge_add_save_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ta = lv_obj_get_child(g_view, 1);
    const char *url = lv_textarea_get_text(ta);
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        lv_obj_t *st = lv_obj_get_child(g_view, 0);
        lv_label_set_text(st, "地址要以 http:// 开头");
        lv_obj_set_style_text_color(st, UI_ERR, 0);
        return;
    }
    ds_bridge_host_save(url);
    ds_bridge_rebind(url);
    bridge_list_refresh();
    bridge_list_view_build();
}

static void bridge_add_view_build(void)
{
    if (g_view) {
        lv_obj_delete(g_view);
        g_view = NULL;
    }
    g_view = lv_obj_create(g_mask);
    lv_obj_set_size(g_view, 480, 480);
    lv_obj_set_style_bg_opa(g_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_view, 0, 0);
    lv_obj_set_style_pad_all(g_view, 0, 0);
    lv_obj_clear_flag(g_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *st = panel_label(g_view, "输入新的 Bridge 地址（保存即切换）",
                               &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 16, 46);

    lv_obj_t *ta = lv_textarea_create(g_view);
    lv_obj_set_size(ta, 440, 54);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 74);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 158);
    if (s_bh_cur[0]) lv_textarea_set_text(ta, s_bh_cur);

    lv_obj_t *save = lv_button_create(g_view);
    lv_obj_set_size(save, 120, 44);
    lv_obj_align(save, LV_ALIGN_TOP_LEFT, 20, 140);
    lv_obj_add_event_cb(save, bridge_add_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l1 = panel_label(save, "保存", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(l1, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *back = lv_button_create(g_view);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 152, 140);
    lv_obj_add_event_cb(back, bridge_list_view_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l2 = panel_label(back, "返回", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(l2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *kb = lv_keyboard_create(g_view);
    lv_obj_set_size(kb, 480, 240);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
}

/* 列表视图：地址簿 + 添加/关闭 */
static void bridge_list_view_build(void)
{
    if (g_view) {
        lv_obj_delete(g_view);
        g_view = NULL;
    }
    bridge_list_refresh();

    g_view = lv_obj_create(g_mask);
    lv_obj_set_size(g_view, 460, 340);
    lv_obj_align(g_view, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(g_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_view, 0, 0);
    lv_obj_set_style_pad_all(g_view, 4, 0);
    lv_obj_set_flex_flow(g_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_view, 6, 0);

    if (s_bh_n == 0) {
        lv_obj_t *em = panel_label(g_view, "还没有保存的地址，点\"添加地址\"",
                                   &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_set_style_pad_top(em, 30, 0);
    }
    for (int i = 0; i < s_bh_n; i++) {
        bool cur = strcmp(s_bh_rows[i], s_bh_cur) == 0;
        lv_obj_t *row = lv_button_create(g_view);
        lv_obj_set_size(row, LV_PCT(100), 46);
        lv_obj_add_event_cb(row, bridge_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *u = panel_label(row, s_bh_rows[i], &lv_font_montserrat_14,
                                  cur ? UI_ACCENT : UI_TEXT);
        lv_obj_align(u, LV_ALIGN_LEFT_MID, 8, 0);
        lv_label_set_long_mode(u, LV_LABEL_LONG_DOT);
        lv_obj_set_width(u, 340);
        if (cur) {
            lv_obj_t *ok = panel_label(row, LV_SYMBOL_OK, &lv_font_montserrat_14, UI_ACCENT);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -10, 0);
        } else {
            lv_obj_t *del = lv_button_create(row); /* 子按钮点它=删除 */
            lv_obj_set_size(del, 36, 36);
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);
            lv_obj_set_style_bg_opa(del, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(del, 0, 0);
            lv_obj_add_event_cb(del, bridge_del_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_t *x = panel_label(del, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, UI_TEXT_DIM);
            lv_obj_align(x, LV_ALIGN_CENTER, 0, 0);
        }
    }

    lv_obj_t *add = lv_button_create(g_mask);
    lv_obj_set_size(add, 190, 46);
    lv_obj_align(add, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    lv_obj_add_event_cb(add, bridge_add_view_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = panel_label(add, "添加地址", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(al, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *close = lv_button_create(g_mask);
    lv_obj_set_size(close, 190, 46);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
    lv_obj_add_event_cb(close, mask_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = panel_label(close, "关闭", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(cl, LV_ALIGN_CENTER, 0, 0);
}

void ui_bridge_host_panel_open(void)
{
    if (g_mask) mask_close_cb(NULL);
    mask_create("Bridge 地址");
    bridge_list_view_build();
}
