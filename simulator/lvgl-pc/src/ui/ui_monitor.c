/* ui_monitor — 服务器监控页（PLAN §5.8 设备端摘要视图）：
 * 每台服务器一张紧凑卡片：名称 + 在线点 + CPU/内存迷你条 + 运行时长。
 * 数据来自 data_source（Bridge Komari 聚合或 mock）。页面退出即销毁
 * （app_manager 资源模型），无驻留定时器。
 */
#include <stdio.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui_theme.h"

/* 刷新按钮事件 → 数据层刷新后重建整页（简单且无状态残留） */
static void monitor_refresh_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *scr = lv_obj_get_parent(btn);
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);

    ds_refresh();
    app_manager_open(app_id); /* 重建（旧屏幕转场后自动销毁） */
    (void)scr;
}

static lv_obj_t *monitor_topbar(lv_obj_t *scr)
{
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    app_manager_back_on_click(back);
    lv_obj_t *icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(icon);
    return back;
}

void ui_monitor_build(lv_obj_t *scr, int app_id)
{
    lv_obj_t *title = ui_label(scr, "服务器监控", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    monitor_topbar(scr);

    const gadget_state_t *s = ds_state();

    /* 头部摘要：在线 x/y + 刷新按钮 */
    int online = 0;
    for (int i = 0; i < s->server_count; i++) {
        if (s->servers[i].online) online++;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "在线 %d/%d · 来源 %s", online, s->server_count, ds_source_name());
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
    lv_obj_set_size(list, LV_PCT(100), 300);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);

    for (int i = 0; i < s->server_count && i < DS_SERVER_MAX; i++) {
        const gadget_server_t *srv = &s->servers[i];
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        ui_style_card(card);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* 行 1：名称 + 在线状态 */
        lv_obj_t *dot = lv_obj_create(card);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, srv->online ? UI_OK : UI_ERR, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 0, 2);

        lv_obj_t *name = ui_label(card, srv->name, &lv_font_cn_14, UI_TEXT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 18, -2);

        snprintf(buf, sizeof(buf), srv->online ? "%d天" : "离线", srv->uptime_h / 24);
        lv_obj_t *up = ui_label(card, buf, &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(up, LV_ALIGN_TOP_RIGHT, 0, 0);

        if (srv->online) {
            /* 行 2/3：CPU/内存迷你条 */
            const struct { const char *label; int pct; int y; } rows[] = {
                { "CPU", srv->cpu_pct, -26 },
                { "MEM", srv->mem_pct, -4 },
            };
            for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
                lv_obj_t *lbl = ui_label(card, rows[r].label, &lv_font_montserrat_12, UI_TEXT_DIM);
                lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, rows[r].y);

                lv_obj_t *bar = lv_bar_create(card);
                lv_obj_set_size(bar, 150, 10);
                lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 42, rows[r].y + 3);
                lv_bar_set_range(bar, 0, 100);
                lv_bar_set_value(bar, rows[r].pct, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(bar, UI_CARD_HI, 0);
                lv_obj_set_style_bg_color(bar, ui_pct_color(rows[r].pct), LV_PART_INDICATOR);
                lv_obj_set_style_radius(bar, 5, 0);
                lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);

                char pb[12];
                snprintf(pb, sizeof(pb), "%d%%", rows[r].pct);
                lv_obj_t *pct_lbl = ui_label(card, pb, &lv_font_montserrat_12, UI_TEXT_DIM);
                lv_obj_align(pct_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, rows[r].y);
            }
        }
    }
}
