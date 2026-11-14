/* ui_launcher — 桌面（定稿图 assistant-ui-final.png 桌面面板）：
 * 波浪渐变背景（canvas 一次性预渲染）+ 状态栏（时间/Wi-Fi/电池）
 * + 3×3 App 磁贴（三 AI 形象头像磁贴 + 彩色符号磁贴）+ 页面指示器。
 * 数据来自 data_source（mock 或 Bridge）；Launcher 屏幕常驻（系统 UI）。
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_launcher.h"
#include "ui_theme.h"

static lv_obj_t *g_launcher;
static lv_obj_t *g_lbl_clock;
static lv_obj_t *g_lbl_date;

static void clock_timer_cb(lv_timer_t *t);
static void tile_click_cb(lv_event_t *e);

/* ---------------- 波浪渐变背景（canvas 预渲染） ----------------
 * 480×480 RGB565 直写像素（一次性，启动几 ms）：
 * 基底纵向渐变 + 顶部径向光晕 + 底部两层半透明正弦波（定稿图波浪）。 */

#define BG_W 480
#define BG_H 480

static uint16_t rgb565_pack(int r, int g, int b)
{
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 7) | (b >> 3));
}

/* RGB565 低位深渐变会出色带：4×4 Bayer 抖动 ±2 消带（目视验收抓出） */
static const int kBayer4[4][4] = {
    { 0, -2,  1, -1 },
    { -2,  2, -1,  3 },
    { 1, -1,  3, -3 },
    { -1,  3, -3,  2 },
};

static uint16_t rgb565_dither(int r, int g, int b, int x, int y)
{
    int d = kBayer4[y & 3][x & 3];
    return rgb565_pack(r + d, g + d, b + d);
}

static void bg_render(uint16_t *px)
{
    /* 基底：上深蓝 → 下更暗；顶部中心光晕提亮（定稿图氛围） */
    const int top[3]    = { 14, 20, 40 };  /* #0E1428 */
    const int bottom[3] = { 7, 10, 22 };   /* #070A16 */
    const int glow[3]   = { 42, 56, 98 };  /* 光晕目标色 #2A3862 */

    for (int y = 0; y < BG_H; y++) {
        float ty = (float)y / (BG_H - 1);
        for (int x = 0; x < BG_W; x++) {
            int r = top[0] + (int)((bottom[0] - top[0]) * ty);
            int g = top[1] + (int)((bottom[1] - top[1]) * ty);
            int b = top[2] + (int)((bottom[2] - top[2]) * ty);

            /* 顶部径向光晕（中心 (240, 40)，半径 300 衰减） */
            float dx = x - 240.0f, dy = (y - 40.0f) * 1.6f;
            float d = sqrtf(dx * dx + dy * dy);
            float f = 1.0f - d / 300.0f;
            if (f > 0) {
                f = f * f * 0.9f;
                r += (int)((glow[0] - r) * f);
                g += (int)((glow[1] - g) * f);
                b += (int)((glow[2] - b) * f);
            }
            px[y * BG_W + x] = rgb565_dither(r, g, b, x, y);
        }
    }

    /* 底部两层波浪：波峰下方 alpha 渐入（软边），前层更靠下 */
    const struct {
        int base_y;     /* 波浪中线 */
        int amp;        /* 振幅 */
        int period;     /* 周期 px */
        float phase;
        int col[3];
        int alpha;      /* 最大不透明度 0~255 */
        int ramp;       /* 软边高度 px */
    } waves[] = {
        { BG_H - 118, 16, 470, 0.0f,          { 96, 76, 200 }, 70, 56 },  /* 紫 #604CC8 */
        { BG_H - 80,  20, 540, 2.2f,          { 36, 130, 170 }, 60, 48 }, /* 青 #2482AA */
    };

    for (int w = 0; w < 2; w++) {
        for (int x = 0; x < BG_W; x++) {
            int wy = waves[w].base_y +
                     (int)(waves[w].amp * sinf(2.0f * (float)M_PI * x / waves[w].period +
                                               waves[w].phase));
            for (int y = wy; y < BG_H; y++) {
                float a = (float)(y - wy) / waves[w].ramp;
                if (a > 1) a = 1;
                a *= (float)waves[w].alpha / 255.0f;
                uint16_t c = px[y * BG_W + x];
                int r = (c >> 11) & 0x1F, g = (c >> 7) & 0x3F, b = (c >> 3) & 0x1F;
                r = r * 8; g = g * 4; b = b * 8;
                r += (int)((waves[w].col[0] - r) * a);
                g += (int)((waves[w].col[1] - g) * a);
                b += (int)((waves[w].col[2] - b) * a);
                px[y * BG_W + x] = rgb565_dither(r, g, b, x, y);
            }
        }
    }
}

static void bg_attach(lv_obj_t *parent)
{
    void *buf = malloc(BG_W * BG_H * 2);
    if (!buf) return; /* 内存不足时退回纯色底（不阻塞桌面） */

    bg_render((uint16_t *)buf);

    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, BG_W, BG_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(cv);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    /* buf 归 canvas 所有：屏幕常驻不销毁，无需释放回调 */
}

/* ---------------- 磁贴 ---------------- */

#define TILE_ICON 64  /* 图标区边长（头像/彩色符号块） */

static void tile_add(lv_obj_t *grid, const app_def_t *def, int cell)
{
    lv_obj_t *tile = lv_button_create(grid);
    lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, cell % 3, 1,
                         LV_GRID_ALIGN_STRETCH, cell / 3, 1);
    /* 定稿图：磁贴本身融入背景（半透明玻璃底），图标承载识别度 */
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x141B30), 0);
    lv_obj_set_style_bg_opa(tile, def->enabled ? LV_OPA_80 : LV_OPA_40, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 24, 0);
    lv_obj_set_style_shadow_width(tile, 12, 0);
    lv_obj_set_style_shadow_opa(tile, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)def->id);

    if (def->avatar) {
        /* AI 磁贴：圆角方形头像（88×88 素材缩到 TILE_ICON） */
        lv_obj_t *holder = lv_obj_create(tile);
        lv_obj_set_size(holder, TILE_ICON, TILE_ICON);
        lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_radius(holder, 17, 0);
        lv_obj_set_style_bg_color(holder, UI_CARD, 0);
        lv_obj_set_style_bg_opa(holder, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(holder, 0, 0);
        lv_obj_set_style_pad_all(holder, 0, 0);
        lv_obj_set_style_clip_corner(holder, true, 0);
        lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *img = lv_image_create(holder);
        lv_image_set_src(img, def->avatar);
        lv_obj_set_size(img, TILE_ICON, TILE_ICON);
        lv_image_set_scale(img, (256 * TILE_ICON) / 88);
        lv_obj_center(img);
    } else {
        /* 符号磁贴：彩色圆角方块 + 白色符号 */
        lv_obj_t *holder = lv_obj_create(tile);
        lv_obj_set_size(holder, TILE_ICON, TILE_ICON);
        lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_radius(holder, 17, 0);
        lv_obj_set_style_bg_color(holder, lv_color_hex(def->tile_color), 0);
        lv_obj_set_style_bg_opa(holder, def->enabled ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_border_width(holder, 0, 0);
        lv_obj_set_style_pad_all(holder, 0, 0);
        lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *sym = ui_label(holder, def->symbol, &lv_font_montserrat_28,
                                 lv_color_hex(0xFFFFFF));
        lv_obj_center(sym);
    }

    /* 标签独立区：图标底 72 与标签顶（~78）分离（首次目视验收重叠后修正） */
    lv_obj_t *name = ui_label(tile, def->name, &lv_font_cn_12,
                              def->enabled ? UI_TEXT : UI_TEXT_DIM);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void ui_launcher_build(void)
{
    if (g_launcher) return;

    g_launcher = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_launcher, UI_BG, 0);
    lv_obj_set_style_bg_opa(g_launcher, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_launcher, 16, 0);
    lv_obj_set_style_border_width(g_launcher, 0, 0);
    lv_obj_clear_flag(g_launcher, LV_OBJ_FLAG_SCROLLABLE);

    /* 背景最先挂（其余控件叠于其上） */
    bg_attach(g_launcher);

    /* ---- 状态栏（定稿图：左侧大时间+日期，右侧信号+电池） ---- */
    lv_obj_t *bar = lv_obj_create(g_launcher);
    lv_obj_set_size(bar, LV_PCT(100), 64);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_clock = ui_label(bar, "--:--", &lv_font_montserrat_28, UI_TEXT);
    lv_obj_align(g_lbl_clock, LV_ALIGN_LEFT_MID, 0, -8);

    g_lbl_date = ui_label(bar, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(g_lbl_date, LV_ALIGN_LEFT_MID, 0, 16);

    /* Wi-Fi/电池由全局状态栏 overlay 统一显示（ui_statusbar），此处不再重复 */

    /* ---- App 磁贴网格（3×3，条目来自 app_manager 注册表） ---- */
    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    int count = app_manager_count();
    int rows = (count + 2) / 3;
    /* LVGL grid 只保存 dsc 指针：数组必须 static（栈数组出函数即失效，
     * 表现为整个网格不渲染——目视验收抓出的 bug） */
    static lv_coord_t row_dsc[4];
    row_dsc[0] = row_dsc[1] = row_dsc[2] = LV_GRID_FR(1);
    row_dsc[rows > 3 ? 3 : rows] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t *grid = lv_obj_create(g_launcher);
    lv_obj_set_size(grid, LV_PCT(100), 328);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 14);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_style_pad_all(grid, 6, 0);
    lv_obj_set_style_pad_gap(grid, 14, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < count; i++) {
        const app_def_t *def = app_manager_get(i);
        if (!def) break;
        tile_add(grid, def, i);
    }

    /* ---- 页面指示器（单页占位） ---- */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(g_launcher);
        lv_obj_set_size(dot, i == 0 ? 16 : 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, i == 0 ? UI_ACCENT : UI_CARD, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, (i - 1) * 18, -6);
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
    snprintf(buf, sizeof(buf), "%d月%02d日 周%s", tmv.tm_mon + 1, tmv.tm_mday, week_cn[tmv.tm_wday]);
    lv_label_set_text(g_lbl_date, buf);
}

lv_obj_t *ui_launcher_screen(void)
{
    ui_launcher_build();
    return g_launcher;
}
