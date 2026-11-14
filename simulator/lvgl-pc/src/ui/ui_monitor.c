/* ui_monitor — 服务器监控页（定稿图 dark-ui-concept 监控面板）：
 * 每台服务器一张卡：紫色服务器图标 + 名称/运行时长 + 在线 pill
 * + 四列指标（CPU / 内存 / 磁盘 / 网络 ↑↓）。
 * 数据来自 data_source（Bridge Komari 聚合或 mock）。页面退出即销毁
 * （app_manager 资源模型），无驻留定时器。
 */
#include <stdio.h>
#include <string.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui_theme.h"

/* 刷新按钮事件 → 数据层刷新后重建整页（简单且无状态残留） */
static void monitor_refresh_cb(lv_event_t *e)
{
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);

    ds_refresh();
    app_manager_open(app_id); /* 重建（旧屏幕转场后自动销毁） */
}

static void monitor_topbar(lv_obj_t *scr)
{
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    app_manager_back_on_click(back);
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

/* 四列指标中的一列：标签 + 数值（TOP 对齐，x 为列左缘） */
static void metric_col(lv_obj_t *card, const char *label, const char *value,
                       lv_color_t vcol, int x, int y)
{
    lv_obj_t *l = ui_label(card, label, &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_t *v = ui_label(card, value, &lv_font_montserrat_14, vcol);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, x, y + 17);
}

static void net_fmt(char *buf, int buflen, int kbps)
{
    /* ≥1MB/s 显示 Mb/s（bps 换算），否则 KB/s */
    if (kbps >= 1024) snprintf(buf, (size_t)buflen, "%.1fM", kbps / 1024.0);
    else snprintf(buf, (size_t)buflen, "%dK", kbps);
}

void ui_monitor_build(lv_obj_t *scr, int app_id)
{
    lv_obj_t *title = ui_label(scr, "服务器监控", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    monitor_topbar(scr);

    const gadget_state_t *s = ds_state();

    /* 头部摘要：在线 x/y + 来源（来源名截短到 '(' 前，避免长 URL
     * 钻进右侧刷新按钮底下——目视验收抓出） */
    int online = 0;
    for (int i = 0; i < s->server_count; i++) {
        if (s->servers[i].online) online++;
    }
    char src[24];
    snprintf(src, sizeof(src), "%s", ds_source_name());
    char *paren = strchr(src, '(');
    if (paren) *paren = '\0';
    char buf[48];
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

    /* 服务器卡片列表（可滚动容器） */
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, LV_PCT(100), 302);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);

    for (int i = 0; i < s->server_count && i < DS_SERVER_MAX; i++) {
        const gadget_server_t *srv = &s->servers[i];
        lv_obj_t *card = lv_obj_create(list);
        /* 固定高度：LV_SIZE_CONTENT 对 BOTTOM 对齐子项计算不足，
         * 条形会溢出到相邻卡片（目视验收抓出） */
        lv_obj_set_size(card, LV_PCT(100), srv->online ? 96 : 52);
        ui_style_card(card);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* 行 1：彩色服务器图标 + 名称/运行时长 + 在线 pill */
        lv_obj_t *ic = lv_obj_create(card);
        lv_obj_set_size(ic, 30, 30);
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_radius(ic, 9, 0);
        lv_obj_set_style_bg_color(ic, srv->online ? lv_color_hex(0x9B5DE5) : UI_CARD_HI, 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ic, 0, 0);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ic_sym = ui_label(ic, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_14, UI_TEXT);
        lv_obj_center(ic_sym);

        lv_obj_t *name = ui_label(card, srv->name, &lv_font_cn_14, UI_TEXT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 38, 0);

        snprintf(buf, sizeof(buf), srv->online ? "已运行 %d 天" : "最近无上报", srv->uptime_h / 24);
        lv_obj_t *up = ui_label(card, buf, &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(up, LV_ALIGN_TOP_LEFT, 38, 19);

        pill_add(card, srv->online, srv->online ? 0 : 2);

        /* 行 2：四列指标（CPU / 内存 / 磁盘 / 网络） */
        if (srv->online) {
            char vbuf[4][16];
            snprintf(vbuf[0], sizeof(vbuf[0]), "%d%%", srv->cpu_pct);
            snprintf(vbuf[1], sizeof(vbuf[1]), "%d%%", srv->mem_pct);
            snprintf(vbuf[2], sizeof(vbuf[2]), "%d%%", srv->disk_pct);
            char up_s[10], dn_s[10];
            net_fmt(up_s, sizeof(up_s), srv->net_up_kbps);
            net_fmt(dn_s, sizeof(dn_s), srv->net_down_kbps);
            snprintf(vbuf[3], sizeof(vbuf[3]), "%s/%s", dn_s, up_s);

            /* 网络列值格式 "下行/上行"（K/M 自动选档） */
            const struct { const char *label; const char *value; lv_color_t col; } cols[] = {
                { "CPU",  vbuf[0], ui_pct_color(srv->cpu_pct) },
                { "内存", vbuf[1], ui_pct_color(srv->mem_pct) },
                { "磁盘", vbuf[2], ui_pct_color(srv->disk_pct) },
                { "网络", vbuf[3], UI_ACCENT2 },
            };
            for (int c = 0; c < 4; c++) {
                metric_col(card, cols[c].label, cols[c].value, cols[c].col,
                           38 + c * 100, 44);
            }
        }
    }
}
