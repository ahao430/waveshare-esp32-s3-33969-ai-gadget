/* ui_launcher — 桌面（定稿图 assistant-ui-final.png 桌面面板）：
 * 波浪渐变背景（canvas 一次性预渲染）+ 状态栏（时间/Wi-Fi/电池）
 * + 3×3 App 磁贴（三 AI 形象头像磁贴 + 彩色符号磁贴）+ 页面指示器。
 * 数据来自 data_source（mock 或 Bridge）；Launcher 屏幕常驻（系统 UI）。
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_launcher.h"
#include "ui_gesture.h"
#include "ui_theme.h"

static lv_obj_t *g_launcher;
static lv_obj_t *g_lbl_clock;
static lv_obj_t *g_lbl_date;

static void clock_timer_cb(lv_timer_t *t);
static void tileview_scroll_cb(lv_event_t *e);
static void tile_click_cb(lv_event_t *e);

/* ---------------- 波浪渐变背景（canvas 预渲染） ----------------
 * 480×480 RGB565 直写像素（一次性，启动几 ms）：
 * 基底纵向渐变 + 顶部径向光晕 + 底部两层半透明正弦波（定稿图波浪）。
 * 波浪/光晕颜色取自当前主题（k_ui_themes），自定义背景优先于波浪。 */

#define BG_W 480
#define BG_H 480

#define CUSTOM_BG_PATH "simulator/lvgl-pc/assets/custom-bg.png"

static uint16_t rgb565_pack(int r, int g, int b)
{
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    /* RGB565：R5=bit11-15，G6=bit5-10（<<5，曾误写 <<7 侵入 R 区，
     * G/R 互相污染——橙系主题下暴露为整体偏绿） */
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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

static void split_rgb(uint32_t hex, int out[3])
{
    out[0] = (int)((hex >> 16) & 0xFF);
    out[1] = (int)((hex >> 8) & 0xFF);
    out[2] = (int)(hex & 0xFF);
}

static void bg_render(uint16_t *px)
{
    const ui_theme_def_t *th = &k_ui_themes[ui_theme_current()];

    /* 基底：上深蓝 → 下更暗；顶部中心光晕提亮（主题色） */
    const int top[3]    = { 14, 20, 40 };  /* #0E1428 */
    const int bottom[3] = { 7, 10, 22 };   /* #070A16 */
    int glow[3];
    split_rgb(th->glow, glow);

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

    /* 底部两层波浪：颜色取主题 wave[2] */
    const struct {
        int base_y;     /* 波浪中线 */
        int amp;        /* 振幅 */
        int period;     /* 周期 px */
        float phase;
        int alpha;      /* 最大不透明度 0~255 */
        int ramp;       /* 软边高度 px */
    } waves[] = {
        { BG_H - 118, 16, 470, 0.0f, 70, 56 },
        { BG_H - 80,  20, 540, 2.2f, 60, 48 },
    };

    for (int w = 0; w < 2; w++) {
        int col[3];
        split_rgb(th->wave[w], col);
        for (int x = 0; x < BG_W; x++) {
            int wy = waves[w].base_y +
                     (int)(waves[w].amp * sinf(2.0f * (float)M_PI * x / waves[w].period +
                                               waves[w].phase));
            for (int y = wy; y < BG_H; y++) {
                float a = (float)(y - wy) / waves[w].ramp;
                if (a > 1) a = 1;
                a *= (float)waves[w].alpha / 255.0f;
                uint16_t c = px[y * BG_W + x];
                int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
                r = r * 8; g = g * 4; b = b * 8;
                r += (int)((col[0] - r) * a);
                g += (int)((col[1] - g) * a);
                b += (int)((col[2] - b) * a);
                px[y * BG_W + x] = rgb565_dither(r, g, b, x, y);
            }
        }
    }
}

static void bg_attach(lv_obj_t *parent)
{
    /* 自定义背景优先（Web 上传到 Bridge，ds_appearance_fetch 缓存到本地）；
     * GADGET_BG=wave 强制内置波浪（验收用） */
    const char *force_wave = getenv("GADGET_BG");
    if ((!force_wave || strcmp(force_wave, "wave") != 0)) {
        FILE *f = fopen(CUSTOM_BG_PATH, "rb");
        if (f) {
            fclose(f);
            lv_obj_t *img = lv_image_create(parent);
            lv_image_set_src(img, "A:" CUSTOM_BG_PATH);
            lv_obj_center(img);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
            return;
        }
    }

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

    /* ---- App 分页（原厂范式：一屏 4 个 2×2，左右滑动翻页 + 底部页点） ---- */
    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    lv_obj_t *tv = lv_tileview_create(g_launcher);
    lv_obj_set_size(tv, LV_PCT(100), 360);
    lv_obj_align(tv, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_style_pad_gap(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *dots[UI_DOTS_MAX];
    int count = app_manager_count();
    int pages = (count + 3) / 4;

    for (int pg = 0; pg < pages; pg++) {
        lv_obj_t *tile = lv_tileview_add_tile(tv, pg, 0, LV_DIR_HOR);
        lv_obj_set_style_pad_all(tile, 8, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *grid = lv_obj_create(tile);
        lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);
        lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
        lv_obj_set_style_pad_all(grid, 0, 0);
        lv_obj_set_style_pad_gap(grid, 12, 0);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 2);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(grid, 0, 0);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

        for (int k = 0; k < 4; k++) {
            int idx = pg * 4 + k;
            if (idx >= count) break;
            const app_def_t *def = app_manager_get(idx);
            if (!def) break;
            lv_obj_t *tile_btn = lv_button_create(grid);
            lv_obj_set_grid_cell(tile_btn, LV_GRID_ALIGN_STRETCH, k % 2, 1,
                                 LV_GRID_ALIGN_STRETCH, k / 2, 1);
            /* 无磁贴卡片底：图标 + 标签直接浮于壁纸（原厂范式） */
            lv_obj_set_style_bg_opa(tile_btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(tile_btn, 0, 0);
            lv_obj_set_style_shadow_width(tile_btn, 0, 0);
            lv_obj_set_style_pad_all(tile_btn, 0, 0);
            lv_obj_add_event_cb(tile_btn, tile_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)def->id);

            const int ICON = 108;
            /* 素材缺位时回退符号占位（assets/icons/ 自备图标体系） */
            bool has_icon = false;
            if (def->avatar) {
                FILE *f = fopen(def->avatar + (strncmp(def->avatar, "A:", 2) == 0 ? 2 : 0), "rb");
                if (f) { fclose(f); has_icon = true; }
            }
            if (has_icon) {
                lv_obj_t *holder = lv_obj_create(tile_btn);
                lv_obj_set_size(holder, ICON, ICON);
                lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, 10);
                lv_obj_set_style_radius(holder, 26, 0);
                lv_obj_set_style_bg_color(holder, UI_CARD, 0);
                lv_obj_set_style_bg_opa(holder, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(holder, 0, 0);
                lv_obj_set_style_pad_all(holder, 0, 0);
                lv_obj_set_style_clip_corner(holder, true, 0);
                lv_obj_set_style_shadow_width(holder, 18, 0);
                lv_obj_set_style_shadow_opa(holder, LV_OPA_40, 0);
                lv_obj_set_style_shadow_color(holder, lv_color_hex(0x000000), 0);
                lv_obj_set_style_shadow_ofs_y(holder, 6, 0);
                lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_t *img = lv_image_create(holder);
                lv_image_set_src(img, def->avatar);
                /* 按源图实际宽度自适应缩放（88 头像/更大自备图标均可） */
                lv_obj_update_layout(img);
                int32_t src_w = lv_obj_get_width(img);
                if (src_w <= 0) src_w = 88;
                lv_obj_set_size(img, ICON, ICON);
                lv_image_set_scale(img, (256 * ICON) / src_w);
                lv_obj_center(img);
            } else {
                lv_obj_t *holder = lv_obj_create(tile_btn);
                lv_obj_set_size(holder, ICON, ICON);
                lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, 10);
                lv_obj_set_style_radius(holder, 26, 0);
                lv_obj_set_style_bg_color(holder, lv_color_hex(def->tile_color), 0);
                lv_obj_set_style_bg_opa(holder, def->enabled ? LV_OPA_COVER : LV_OPA_40, 0);
                lv_obj_set_style_border_width(holder, 0, 0);
                lv_obj_set_style_pad_all(holder, 0, 0);
                lv_obj_set_style_shadow_width(holder, 18, 0);
                lv_obj_set_style_shadow_opa(holder, LV_OPA_40, 0);
                lv_obj_set_style_shadow_color(holder, lv_color_hex(0x000000), 0);
                lv_obj_set_style_shadow_ofs_y(holder, 6, 0);
                lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_t *sym = ui_label(holder, def->symbol, &lv_font_montserrat_28,
                                         lv_color_hex(0xFFFFFF));
                lv_obj_center(sym);
            }
            lv_obj_t *name = ui_label(tile_btn, def->name, &lv_font_cn_14,
                                      def->enabled ? UI_TEXT : UI_TEXT_DIM);
            lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 128);
        }
    }

    /* 页点（tileview 滚动联动） */
    static lv_obj_t *g_page_dots[UI_DOTS_MAX];
    ui_dots_create(g_launcher, g_page_dots, UI_DOTS_MAX, pages, LV_ALIGN_BOTTOM_MID, -8);
    lv_obj_add_event_cb(tv, tileview_scroll_cb, LV_EVENT_SCROLL, g_page_dots);

        /* 时钟定时器：rebuild 时先删旧    /* 时钟定时器：rebuild 时先删旧（timer 不随屏幕对象释放） */
    static lv_timer_t *clock_timer;
    if (clock_timer) lv_timer_delete(clock_timer);
    clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
}

static void tileview_scroll_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t **dots = (lv_obj_t **)lv_event_get_user_data(e);
    int pages = (app_manager_count() + 3) / 4;
    int cur = ui_page_of_scroll(tv, 480); /* tile 宽 = 屏宽 */
    if (cur < 0) cur = 0;
    if (cur >= pages) cur = pages - 1;
    ui_dots_update(dots, pages, cur);
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

void ui_launcher_rebuild(void)
{
    if (!g_launcher) return;
    bool active = (lv_screen_active() == g_launcher);

    lv_obj_delete(g_launcher);
    g_launcher = NULL; /* 时钟定时器在 build 内自删自建 */

    ui_launcher_build();
    app_manager_set_home(g_launcher);
    if (active) lv_screen_load(g_launcher);
}
