/* ui_monitor — 服务器监控页（ADR-008 横滑范式）：
 * 列表 = 紧凑行卡（点击进详情）；详情 = 每服务器一 tile 横滑 + 页点。
 * 无返回按钮：列表上滑退出 App；详情上滑回列表（层级返回）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_gesture.h"
#include "ui_theme.h"

/* -1 = 列表；>=0 = 详情（tileview 初始定位） */
static int g_detail = -1;
static lv_obj_t *g_dots[UI_DOTS_MAX];

static void monitor_refresh_cb(lv_event_t *e)
{
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);
    ds_refresh();
    app_manager_open(app_id); /* 重建（旧屏幕转场后自动销毁） */
}

static void row_click_cb(lv_event_t *e)
{
    g_detail = (int)(uintptr_t)lv_event_get_user_data(e);
    app_manager_open(UI_APP_MONITOR); /* 重建为详情 */
}

static void detail_back_to_list(void *unused)
{
    (void)unused;
    g_detail = -1;
    app_manager_open(UI_APP_MONITOR);
}

static void monitor_refresh_btn(lv_obj_t *scr)
{
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 96, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 0, 28);
    ui_style_card(btn);
    lv_obj_set_style_bg_color(btn, UI_ACCENT, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, monitor_refresh_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_APP_MONITOR);
    lv_obj_t *lbl = ui_label(btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(lbl);
}

static void net_fmt(char *buf, int buflen, int kbps)
{
    if (kbps >= 1024) snprintf(buf, (size_t)buflen, "%.1fM", kbps / 1024.0);
    else snprintf(buf, (size_t)buflen, "%dK", kbps);
}

static void pill_add(lv_obj_t *card, bool online)
{
    lv_obj_t *pill = lv_obj_create(card);
    lv_obj_set_size(pill, 58, 22);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_radius(pill, 11, 0);
    lv_obj_set_style_bg_color(pill, online ? UI_OK : UI_ERR, 0);
    lv_obj_set_style_bg_opa(pill, online ? LV_OPA_30 : LV_OPA_20, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *txt = ui_label(pill, online ? "在线" : "离线", &lv_font_cn_12,
                             online ? UI_OK : UI_ERR);
    lv_obj_center(txt);
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
    ui_gesture_back_add(scr); /* 上滑退出 App */
    lv_obj_t *title = ui_label(scr, "服务器监控", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    monitor_refresh_btn(scr);

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
            snprintf(buf, sizeof(buf), "CPU %d%% · 内存 %d%% · %s↓ %s↑",
                     srv->cpu_pct, srv->mem_pct, dn, up);
        } else {
            snprintf(buf, sizeof(buf), "最近无上报");
        }
        lv_obj_t *sum = ui_label(card, buf, &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(sum, LV_ALIGN_TOP_LEFT, 54, 30);

        pill_add(card, srv->online);
    }
}

/* ---------------- 详情模式（每服务器一 tile 横滑） ---------------- */

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

static void detail_tile_build(lv_obj_t *tile, const gadget_server_t *srv)
{
    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, LV_PCT(100), 330);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 60);
    ui_style_card(card);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    server_icon(card, srv->online, 0, 0, 44);
    lv_obj_t *name = ui_label(card, srv->name, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 56, 0);
    char buf[48];
    snprintf(buf, sizeof(buf), srv->online ? "已运行 %d 天" : "最近无上报",
             srv->uptime_h / 24);
    lv_obj_t *up = ui_label(card, buf, &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(up, LV_ALIGN_TOP_LEFT, 56, 26);
    pill_add(card, srv->online);

    if (srv->online) {
        char vb[4][16];
        snprintf(vb[0], sizeof(vb[0]), "%d%%", srv->cpu_pct);
        snprintf(vb[1], sizeof(vb[1]), "%d%%", srv->mem_pct);
        snprintf(vb[2], sizeof(vb[2]), "%d%%", srv->disk_pct);
        char us[10], ds[10];
        net_fmt(us, sizeof(us), srv->net_up_kbps);
        net_fmt(ds, sizeof(ds), srv->net_down_kbps);
        snprintf(vb[3], sizeof(vb[3]), "%s↓ %s↑", ds, us);

        metric_bar(card, "CPU",  srv->cpu_pct,  vb[0], 64);
        metric_bar(card, "内存", srv->mem_pct,  vb[1], 116);
        metric_bar(card, "磁盘", srv->disk_pct, vb[2], 168);
        /* 网络：无百分比语义，纯数值行 */
        lv_obj_t *nl = ui_label(card, "网络", &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 0, 220);
        lv_obj_t *nv = ui_label(card, vb[3], &lv_font_cn_12, UI_ACCENT2);
        lv_obj_align(nv, LV_ALIGN_TOP_RIGHT, 0, 220);
    } else {
        lv_obj_t *hint = ui_label(card, "该服务器当前离线", &lv_font_cn_14, UI_TEXT_DIM);
        lv_obj_center(hint);
    }
}

static void detail_scroll_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    int cur = ui_page_of_scroll(tv, 0); /* 步长实测 */
    const gadget_state_t *s = ds_state();
    if (cur < 0) cur = 0;
    if (cur >= s->server_count) cur = s->server_count - 1;
    ui_dots_update(g_dots, s->server_count, cur);
}

void ui_monitor_build(lv_obj_t *scr, int app_id)
{
    /* 验收钩子：GADGET_DEMO_MONITOR_DETAIL=n 直达详情 */
    const char *dt = getenv("GADGET_DEMO_MONITOR_DETAIL");
    if (dt && dt[0]) g_detail = atoi(dt);

    const gadget_state_t *s = ds_state();
    if (g_detail < 0) {
        build_list(scr, s, app_id);
        return;
    }

    /* 详情 = tileview 横滑（每服务器一 tile），上滑回列表 */
    ui_gesture_back_add_ex(scr, detail_back_to_list, NULL);
    lv_obj_t *title = ui_label(scr, "服务器 · 横滑查看", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    monitor_refresh_btn(scr);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_size(tv, 480, 380);
    lv_obj_align(tv, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < s->server_count && i < DS_SERVER_MAX; i++) {
        lv_obj_t *tile = lv_tileview_add_tile(tv, i, 0, LV_DIR_HOR);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
        detail_tile_build(tile, &s->servers[i]);
    }
    /* 初始定位到点击的服务器（set_tile_by_index 安全；scroll_to_view
     * 在布局完成前调用曾崩——lv_tileview_get_tile_active 返回未就绪指针） */
    lv_tileview_set_tile_by_index(tv, (uint32_t)g_detail, 0, LV_ANIM_OFF);

    ui_dots_create(scr, g_dots, UI_DOTS_MAX, s->server_count,
                   LV_ALIGN_BOTTOM_MID, -8);
    ui_dots_update(g_dots, s->server_count, g_detail); /* set_tile 早于 cb 挂载 */
    lv_obj_add_event_cb(tv, detail_scroll_cb, LV_EVENT_SCROLL, NULL);
}
