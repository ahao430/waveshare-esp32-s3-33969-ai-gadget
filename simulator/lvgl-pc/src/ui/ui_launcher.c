/* ui_launcher — 桌面：状态栏（时间/Wi-Fi/电池）+ App 网格（来自 app_manager 注册表）+ 页面指示器。
 * 数据来自 data_source（mock 或 Bridge）；Launcher 屏幕常驻（系统 UI）。
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_launcher.h"
#include "ui_theme.h"

static lv_obj_t *g_launcher;
static lv_obj_t *g_lbl_clock;
static lv_obj_t *g_lbl_date;
static lv_obj_t *g_lbl_glance;
static lv_obj_t *g_lbl_battery;
static lv_obj_t *g_lbl_wifi;

static void clock_timer_cb(lv_timer_t *t);
static void tile_click_cb(lv_event_t *e);

void ui_launcher_build(void)
{
    if (g_launcher) return;

    g_launcher = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_launcher, UI_BG, 0);
    lv_obj_set_style_bg_opa(g_launcher, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_launcher, 16, 0);
    lv_obj_set_style_border_width(g_launcher, 0, 0);
    lv_obj_clear_flag(g_launcher, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 状态栏 ---- */
    lv_obj_t *bar = lv_obj_create(g_launcher);
    lv_obj_set_size(bar, LV_PCT(100), 42);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_clock = ui_label(bar, "--:--", &lv_font_montserrat_28, UI_TEXT);
    lv_obj_align(g_lbl_clock, LV_ALIGN_LEFT_MID, 0, 0);

    g_lbl_wifi = ui_label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_16, UI_TEXT_DIM);
    lv_obj_align(g_lbl_wifi, LV_ALIGN_RIGHT_MID, -58, 0);
    g_lbl_battery = ui_label(bar, LV_SYMBOL_BATTERY_FULL " 82%", &lv_font_montserrat_16, UI_TEXT_DIM);
    lv_obj_align(g_lbl_battery, LV_ALIGN_RIGHT_MID, 0, 0);

    g_lbl_date = ui_label(g_launcher, "", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(g_lbl_date, LV_ALIGN_TOP_LEFT, 0, 50);

    /* ---- 额度速览（数据流验证卡片） ---- */
    g_lbl_glance = ui_label(g_launcher, "", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_align(g_lbl_glance, LV_ALIGN_TOP_RIGHT, 0, 50);

    /* ---- App 网格（3×N，条目来自 app_manager） ---- */
    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    int count = app_manager_count();
    int rows = (count + 2) / 3;
    lv_coord_t row_dsc[6] = { 0 };
    for (int r = 0; r < rows && r < 6; r++) row_dsc[r] = LV_GRID_FR(1);
    row_dsc[rows > 5 ? 5 : rows] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t *grid = lv_obj_create(g_launcher);
    lv_obj_set_size(grid, LV_PCT(100), 306);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_gap(grid, 12, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < count; i++) {
        const app_def_t *def = app_manager_get(i);
        if (!def) break;

        lv_obj_t *tile = lv_button_create(grid);
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, i % 3, 1,
                             LV_GRID_ALIGN_STRETCH, i / 3, 1);
        ui_style_card(tile);
        lv_obj_set_style_bg_color(tile, def->enabled ? UI_CARD : UI_BG, 0);
        lv_obj_set_style_border_side(tile, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_border_color(tile, def->enabled ? UI_ACCENT : UI_CARD, 0);
        lv_obj_set_style_radius(tile, 20, 0);
        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)def->id);

        lv_obj_t *icon = ui_label(tile, def->symbol, &lv_font_montserrat_28,
                                  def->enabled ? UI_ACCENT2 : UI_TEXT_DIM);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);

        lv_obj_t *name = ui_label(tile, def->name, &lv_font_cn_14,
                                  def->enabled ? UI_TEXT : UI_TEXT_DIM);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    /* ---- 页面指示器（单页占位） ---- */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(g_launcher);
        lv_obj_set_size(dot, i == 0 ? 16 : 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, i == 0 ? UI_ACCENT : UI_CARD, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, (i - 1) * 18, 0);
    }

    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
}

static void tile_click_cb(lv_event_t *e)
{
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);
    ui_open_app(app_id);
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_launcher || lv_screen_active() != g_launcher) return;

    static const char *week_cn[] = { "日", "一", "二", "三", "四", "五", "六" };

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[48];
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    lv_label_set_text(g_lbl_clock, buf);
    snprintf(buf, sizeof(buf), "%d月%02d日 · 周%s", tmv.tm_mon + 1, tmv.tm_mday, week_cn[tmv.tm_wday]);
    lv_label_set_text(g_lbl_date, buf);

    const gadget_state_t *s = ds_state();
    lv_obj_set_style_text_color(g_lbl_wifi, s->wifi_ok ? UI_TEXT_DIM : UI_ERR, 0);
    snprintf(buf, sizeof(buf), LV_SYMBOL_BATTERY_FULL " %d%%", s->battery_pct);
    lv_label_set_text(g_lbl_battery, buf);

    snprintf(buf, sizeof(buf), "智谱 5小时 %d%% | 本周 %d%%", s->zhipu_used_5h, s->zhipu_used_week);
    lv_label_set_text(g_lbl_glance, buf);
}

lv_obj_t *ui_launcher_screen(void)
{
    ui_launcher_build();
    return g_launcher;
}
