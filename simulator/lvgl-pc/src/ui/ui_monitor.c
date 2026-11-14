/* ui_monitor — 服务器监控页（用户 2026-11-14 反馈重构：不再滚动）：
 * 列表模式 = 每台服务器一张紧凑行卡（图标/名称/在线胶囊/一行摘要），
 * 点击进入详情模式 = 单服务器大卡（四列指标 + 横向条 + ‹ › 轮播切换，
 * back 回列表）。数据来自 data_source（Bridge Komari 聚合或 mock）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h" /* UI_APP_MONITOR */
#include "ui_theme.h"

/* -1 = 列表模式；>=0 = 详情模式显示该下标服务器 */
static int g_detail = -1;

/* 刷新按钮事件 → 数据层刷新后重建整页（简单且无状态残留） */
static void monitor_refresh_cb(lv_event_t *e)
{
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);

    ds_refresh();
    app_manager_open(app_id); /* 重建（旧屏幕转场后自动销毁） */
}

static void row_click_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    g_detail = idx;
    app_manager_open(UI_APP_MONITOR); /* 重建为详情模式 */
}

static void detail_prev_cb(lv_event_t *e)
{
    (void)e;
    const gadget_state_t *s = ds_state();
    if (s->server_count > 0) {
        g_detail = (g_detail - 1 + s->server_count) % s->server_count;
    }
    app_manager_open(UI_APP_MONITOR);
}

static void detail_next_cb(lv_event_t *e)
{
    (void)e;
    const gadget_state_t *s = ds_state();
    if (s->server_count > 0) {
        g_detail = (g_detail + 1) % s->server_count;
    }
    app_manager_open(UI_APP_MONITOR);
}

/* 详情模式 back：回列表（再 back 才回 Launcher） */
static void detail_back_cb(lv_event_t *e)
{
    (void)e;
    g_detail = -1;
    app_manager_open(UI_APP_MONITOR);
}

static void net_fmt(char *buf, int buflen, int kbps)
{
    if (kbps >= 1024) snprintf(buf, (size_t)buflen, "%.1fM", kbps / 1024.0);
    else snprintf(buf, (size_t)buflen, "%dK", kbps);
}

static void monitor_topbar(lv_obj_t *scr, bool detail_mode)
{
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 28); /* 避开全局状态栏 */
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    if (detail_mode) {
        lv_obj_add_event_cb(back, detail_back_cb, LV_EVENT_CLICKED, NULL);
    } else {
        app_manager_back_on_click(back);
    }
    lv_obj_t *icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(icon);
}

/* 在线/离线胶囊 */
static void pill_add(lv_obj_t *card, bool online, int y)
{
    lv_obj_t *pill = lv_obj_create(card);
    lv_obj_set_size(pill, 58, 22);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, 0, y);
    lv_obj_set_style_radius(pill, 11, 0);
    lv_obj_set_style_bg_color(pill, online ? UI_OK : UI_ERR, 0);
    lv_obj_set_style_bg_opa(pill, online ? LV_OPA_30 : LV_OPA_20, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(pill);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, online ? UI_OK : UI_ERR, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);

    lv_obj_t *txt = ui_label(pill, online ? "在线" : "离线", &lv_font_cn_12,
                             online ? UI_OK : UI_ERR);
    lv_obj_align(txt, LV_ALIGN_LEFT_MID, 16, 0);
}

static lv_obj_t *server_icon(lv_obj_t *parent, bool online, int x, int y, int size)
{
    lv_obj_t *ic = lv_obj_create(parent);
    lv_obj_set_size(ic, size, size);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(ic, size / 3, 0);
    lv_obj_set_style_bg_color(ic, online ? lv_color_hex(0x9B5DE5) : UI_CARD_HI, 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_pad_all(ic, 0, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sym = ui_label(ic, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(sym);
    return ic;
}

/* ---------------- 列表模式 ---------------- */

static void build_list(lv_obj_t *scr, const gadget_state_t *s, int app_id)
{
    lv_obj_t *title = ui_label(scr, "服务器监控", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    monitor_topbar(scr, false);

    int online = 0;
    for (int i = 0; i < s->server_count; i++) {
        if (s->servers[i].online) online++;
    }
    char src[24], buf[64];
    snprintf(src, sizeof(src), "%s", ds_source_name());
    char *paren = strchr(src, '(');
    if (paren) *paren = '\0';
    snprintf(buf, sizeof(buf), "在线 %d/%d · 来源 %s", online, s->server_count, src);
    lv_obj_t *summary = ui_label(scr, buf, &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(summary, LV_ALIGN_TOP_LEFT, 8, 52);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 96, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 0, 48);
    ui_style_card(btn);
    lv_obj_set_style_bg_color(btn, UI_ACCENT, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, monitor_refresh_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)app_id);
    lv_obj_t *btn_lbl = ui_label(btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(btn_lbl);

    /* 紧凑行卡列表（6 台内一屏放下，无需滚动） */
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 340);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);

    for (int i = 0; i < s->server_count && i < DS_SERVER_MAX; i++) {
        const gadget_server_t *srv = &s->servers[i];
        lv_obj_t *card = lv_button_create(list);
        lv_obj_set_size(card, LV_PCT(100), 54);
        ui_style_card(card);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_add_event_cb(card, row_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        server_icon(card, srv->online, 10, 11, 32);

        lv_obj_t *name = ui_label(card, srv->name, &lv_font_cn_16, UI_TEXT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 54, 6);

        char up[10], dn[10];
        net_fmt(up, sizeof(up), srv->net_up_kbps);
        net_fmt(dn, sizeof(dn), srv->net_down_kbps);
        if (srv->online) {
            snprintf(buf, sizeof(buf), "CPU %d%% · 内存 %d%% · ↓%s ↑%s",
                     srv->cpu_pct, srv->mem_pct, dn, up);
        } else {
            snprintf(buf, sizeof(buf), "最近无上报");
        }
        lv_obj_t *sum = ui_label(card, buf, &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(sum, LV_ALIGN_TOP_LEFT, 54, 30);

        /* 小胶囊（行内右侧） */
        lv_obj_t *pill = lv_obj_create(card);
        lv_obj_set_size(pill, 50, 20);
        lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -8, 16);
        lv_obj_set_style_radius(pill, 10, 0);
        lv_obj_set_style_bg_color(pill, srv->online ? UI_OK : UI_ERR, 0);
        lv_obj_set_style_bg_opa(pill, srv->online ? LV_OPA_30 : LV_OPA_20, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        lv_obj_set_style_pad_all(pill, 0, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *pt = ui_label(pill, srv->online ? "在线" : "离线",
                                &lv_font_cn_12, srv->online ? UI_OK : UI_ERR);
        lv_obj_center(pt);
    }
}

/* ---------------- 详情模式（单服务器大卡 + ‹ › 轮播） ---------------- */

/* 横向指标条（标签 + bar + 百分比/数值） */
static void metric_bar(lv_obj_t *card, const char *label, int pct,
                       const char *value, int y)
{
    lv_obj_t *l = ui_label(card, label, &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_t *v = ui_label(card, value, &lv_font_cn_12, UI_TEXT);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, y);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, LV_PCT(100), 8);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y + 20);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, UI_CARD_HI, 0);
    lv_obj_set_style_bg_color(bar, ui_pct_color(pct), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
}

static void build_detail(lv_obj_t *scr, const gadget_state_t *s)
{
    if (g_detail < 0 || g_detail >= s->server_count) g_detail = 0;
    const gadget_server_t *srv = &s->servers[g_detail];

    char buf[48];
    snprintf(buf, sizeof(buf), "服务器 · %d/%d", g_detail + 1, s->server_count);
    lv_obj_t *title = ui_label(scr, buf, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    monitor_topbar(scr, true);

    /* ‹ › 轮播：置于大卡两侧垂直居中（与 back 语义分离） */
    lv_obj_t *prev = lv_button_create(scr);
    lv_obj_set_size(prev, 44, 44);
    lv_obj_align(prev, LV_ALIGN_LEFT_MID, 0, 26);
    ui_style_card(prev);
    lv_obj_set_style_radius(prev, 14, 0);
    lv_obj_add_event_cb(prev, detail_prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pi = ui_label(prev, LV_SYMBOL_LEFT, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(pi);

    lv_obj_t *next = lv_button_create(scr);
    lv_obj_set_size(next, 44, 44);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 26);
    ui_style_card(next);
    lv_obj_set_style_radius(next, 14, 0);
    lv_obj_add_event_cb(next, detail_next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ni = ui_label(next, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(ni);

    /* 大卡 */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LV_PCT(100), 330);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -8);
    ui_style_card(card);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    server_icon(card, srv->online, 0, 0, 44);
    lv_obj_t *name = ui_label(card, srv->name, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 56, 0);
    snprintf(buf, sizeof(buf), srv->online ? "已运行 %d 天" : "最近无上报",
             srv->uptime_h / 24);
    lv_obj_t *up = ui_label(card, buf, &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(up, LV_ALIGN_TOP_LEFT, 56, 26);
    pill_add(card, srv->online, 4);

    if (srv->online) {
        char vb[4][16];
        snprintf(vb[0], sizeof(vb[0]), "%d%%", srv->cpu_pct);
        snprintf(vb[1], sizeof(vb[1]), "%d%%", srv->mem_pct);
        snprintf(vb[2], sizeof(vb[2]), "%d%%", srv->disk_pct);
        char us[10], ds[10];
        net_fmt(us, sizeof(us), srv->net_up_kbps);
        net_fmt(ds, sizeof(ds), srv->net_down_kbps);
        snprintf(vb[3], sizeof(vb[3]), "↓%s ↑%s", ds, us);

        int net_pct = srv->net_down_kbps * 100 / 4096; /* 以 4M 为满条 */
        if (net_pct > 100) net_pct = 100;
        metric_bar(card, "CPU",  srv->cpu_pct,  vb[0], 64);
        metric_bar(card, "内存", srv->mem_pct,  vb[1], 116);
        metric_bar(card, "磁盘", srv->disk_pct, vb[2], 168);
        /* 网络：无百分比语义，纯数值行（不带 bar） */
        lv_obj_t *nl = ui_label(card, "网络", &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 0, 220);
        lv_obj_t *nv = ui_label(card, vb[3], &lv_font_cn_12, UI_ACCENT2);
        lv_obj_align(nv, LV_ALIGN_TOP_RIGHT, 0, 220);

        lv_obj_t *hint = ui_label(card, "‹ › 切换服务器 · 返回键回列表",
                                  &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    } else {
        lv_obj_t *hint = ui_label(card, "该服务器当前离线\n‹ › 切换 · 返回键回列表",
                                  &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_center(hint);
    }
}

void ui_monitor_build(lv_obj_t *scr, int app_id)
{
    /* 验收钩子：GADGET_DEMO_MONITOR_DETAIL=n 直达第 n 台详情（轮播页） */
    const char *dt = getenv("GADGET_DEMO_MONITOR_DETAIL");
    if (dt && dt[0]) g_detail = atoi(dt);

    const gadget_state_t *s = ds_state();
    if (g_detail < 0) build_list(scr, s, app_id);
    else build_detail(scr, s);
}
