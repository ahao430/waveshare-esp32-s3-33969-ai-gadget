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

/* LVGL 9.x 把 lv_image_decoder_dsc_t 放在私有头（9.2/9.4 字段一致，
 * 均含 decoded 指针）；自定义背景一次性解码需要它 */
#include "src/draw/lv_image_decoder_private.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "icon_embed.h" /* 真机图标：嵌入固件（gen_icon_embed.sh 预解码 RGB565） */
#endif

static lv_obj_t *g_launcher;
static lv_obj_t *g_lbl_clock;
static lv_obj_t *g_lbl_date;

static void clock_timer_cb(lv_timer_t *t);
static void tileview_scroll_cb(lv_event_t *e);
static int s_last_page = -1; /* 页点缓存（仅翻页时更新） */
static lv_obj_t *s_tileview;   /* 当前桌面 tileview */

/* ---- 桌面快滑翻页（替代手指拖动，见 build 里禁用 SCROLLABLE 的注释）---- */
#define SWIPE_MIN_DX 50   /* 横向位移下限 */
#define SWIPE_MAX_MS 500  /* 快速滑动窗口 */
static lv_indev_t *s_swipe_indev;
static lv_timer_t *s_swipe_timer;
static bool s_swipe_pressed;
static lv_point_t s_swipe_start, s_swipe_last;
static uint32_t s_swipe_t0;
static bool s_swipe_consumed; /* 本次按压已被判定为翻页 → 抑制打开 */

static void launcher_swipe_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_swipe_indev) {
        for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
            if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) { s_swipe_indev = i; break; }
        }
        if (!s_swipe_indev) return;
    }
    lv_point_t p;
    if (lv_indev_get_state(s_swipe_indev) == LV_INDEV_STATE_PRESSED) {
        lv_indev_get_point(s_swipe_indev, &p);
        if (!s_swipe_pressed) {
            s_swipe_pressed = true;
            s_swipe_start = s_swipe_last = p;
            s_swipe_t0 = lv_tick_get();
            s_swipe_consumed = false;
        } else {
            s_swipe_last = p;
        }
    } else if (s_swipe_pressed) {
        s_swipe_pressed = false;
        int dx = s_swipe_last.x - s_swipe_start.x;
        int dy = s_swipe_last.y - s_swipe_start.y;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (adx >= SWIPE_MIN_DX && adx >= 2 * ady &&
            lv_tick_elaps(s_swipe_t0) <= SWIPE_MAX_MS) {
            int pages = (app_manager_count() + 3) / 4;
            int cur = s_last_page < 0 ? 0 : s_last_page;
            int nxt = cur + (dx < 0 ? 1 : -1);
            if (nxt < 0 || nxt >= pages) return; /* 边界：不动 */
            s_swipe_consumed = true;             /* 抑制本次点击 */
            s_last_page = nxt;
            lv_tileview_set_tile_by_index(s_tileview, (uint32_t)nxt, 0, LV_ANIM_OFF);
        }
    }
}
static void tile_press_cb(lv_event_t *e);
static void tile_press_lost_cb(lv_event_t *e);
static void tile_release_cb(lv_event_t *e);

/* ---------------- 波浪渐变背景（canvas 预渲染） ----------------
 * 480×480 RGB565 直写像素（一次性，启动几 ms）：
 * 基底纵向渐变 + 顶部径向光晕 + 底部两层半透明正弦波（定稿图波浪）。
 * 波浪/光晕颜色取自当前主题（k_ui_themes），自定义背景优先于波浪。 */

#define BG_W 480
#define BG_H 480

#define CUSTOM_BG_PATH "simulator/lvgl-pc/assets/custom-bg.png"

static uint16_t rgb565_pack(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
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

/* 自定义背景：加载时一次性解码成 480×480 RGB565 画布缓冲。
 * 不能直接把 PNG 文件交给 lv_image（"A:path"）：480×480 解码结果
 * 460KB 超过 LVGL 图片缓存（256KB）→ 每次重绘都重新解码 PNG，
 * 滑动/转场时直接把显示管线拖死（真机判据：背景越大越卡）。
 * 解码缓冲常驻（launcher 屏幕常驻，不释放；重建极罕见可接受一次泄漏）。 */
static void *g_custom_bg_buf;

static bool custom_bg_canvas(lv_obj_t *parent)
{
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, "A:" CUSTOM_BG_PATH, NULL) != LV_RESULT_OK) {
        return false;
    }
    const lv_draw_buf_t *db = dsc.decoded;
    if (!db || !db->data) {
        lv_image_decoder_close(&dsc);
        return false;
    }
    if (g_custom_bg_buf == NULL) {
        g_custom_bg_buf = malloc(BG_W * BG_H * 2);
    }
    if (g_custom_bg_buf == NULL) {
        lv_image_decoder_close(&dsc);
        return false;
    }

    uint16_t *dst = (uint16_t *)g_custom_bg_buf;
    uint32_t sw = db->header.w, sh = db->header.h;
    uint32_t stride = db->header.stride;
    const uint8_t *src = (const uint8_t *)db->data;

    for (int y = 0; y < BG_H; y++) {
        uint32_t sy = sh ? (uint32_t)y * sh / BG_H : 0;
        if (sy >= sh) sy = sh ? sh - 1 : 0;
        for (int x = 0; x < BG_W; x++) {
            uint32_t sx = sw ? (uint32_t)x * sw / BG_W : 0;
            if (sx >= sw) sx = sw ? sw - 1 : 0;
            uint16_t v = 0;
            if (db->header.cf == LV_COLOR_FORMAT_RGB565) {
                v = *(const uint16_t *)(src + sy * stride + sx * 2);
            } else if (db->header.cf == LV_COLOR_FORMAT_RGB888) {
                const uint8_t *p = src + sy * stride + sx * 3;
                v = rgb565_pack(p[0], p[1], p[2]);
            } else { /* ARGB8888（LE：B,G,R,A）*/
                const uint8_t *p = src + sy * stride + sx * 4;
                v = rgb565_pack(p[2], p[1], p[0]);
            }
            dst[y * BG_W + x] = v;
        }
    }
    lv_image_decoder_close(&dsc);

    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, g_custom_bg_buf, BG_W, BG_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(cv);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return true;
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
            if (custom_bg_canvas(parent)) return;
        }
    }

    /* 波浪背景 buffer 静态复用：ui_launcher_rebuild 会整屏删除，canvas
     * 释放但 buffer 保留给下一次 attach（否则每次重建泄漏 BG_W*BG_H*2） */
    static void *s_wave_buf;
    if (s_wave_buf == NULL) s_wave_buf = malloc(BG_W * BG_H * 2);
    if (!s_wave_buf) return; /* 内存不足时退回纯色底（不阻塞桌面） */

    bg_render((uint16_t *)s_wave_buf);

    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, s_wave_buf, BG_W, BG_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(cv);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
}

/* ---------------- 磁贴 ---------------- */

#define TILE_ICON 64  /* 图标区边长（头像/彩色符号块） */

#ifdef ESP_PLATFORM
/* 图标错峰挂载：首帧 9 张 88×88 ARGB 同帧 blend + canvas 背景会超出
 * 显示管线瞬时能力——首刷必卡死且 TWDT 静默（坑#2 变体，2026-11-15
 * 真机三组对照定位：探针版时序错开即正常）。构建时只出背景，
 * 首帧后 700ms 起 60ms/张渐进 set_src。 */
#define LAZY_ICONS_MAX 12
static struct { lv_obj_t *img; const void *src; } s_lazy[LAZY_ICONS_MAX];
static int s_lazy_n;
static int s_lazy_i;

static void lazy_icon_cb(lv_timer_t *t)
{
    if (s_lazy_i >= s_lazy_n) {
        lv_timer_del(t);
        return;
    }
    /* 素材 108×108 与控件 1:1，只挂 src（无缩放/无重新布局） */
    lv_image_set_src(s_lazy[s_lazy_i].img, s_lazy[s_lazy_i].src);
    s_lazy_i++;
    lv_timer_set_period(t, 40);
}
#endif

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
    lv_obj_add_event_cb(tile, tile_press_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)def->id);

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
    lv_obj_set_size(bar, LV_PCT(100), 76);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 真机目视反馈：状态栏偏小 → 时间 28→48、日期 14→16；
     * 圆屏顶部安全区：x 内缩 10，避免左上角被圆边裁切 */
    g_lbl_clock = ui_label(bar, "--:--", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(g_lbl_clock, LV_ALIGN_LEFT_MID, 10, -13);

    g_lbl_date = ui_label(bar, "", &lv_font_cn_16, UI_TEXT_DIM);
    lv_obj_align(g_lbl_date, LV_ALIGN_LEFT_MID, 12, 24);

    /* Wi-Fi/电池由全局状态栏 overlay 统一显示（ui_statusbar），此处不再重复 */

    /* ---- App 分页（原厂范式：一屏 4 个 2×2，左右滑动翻页 + 底部页点） ---- */
    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    lv_obj_t *tv = lv_tileview_create(g_launcher);
    s_tileview = tv;
    lv_obj_set_size(tv, LV_PCT(100), 360);
    lv_obj_align(tv, LV_ALIGN_CENTER, 0, 16); /* 状态栏加高（76px）后下移 */
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_style_pad_gap(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    /* 禁用手指直接拖动：拖动会让 tileview 每帧全屏重绘（背景图 + 4 图标
     * 合成），持续拖拽把显示管线压垮（真机实测：60 次程序化翻页零问题，
     * 手指连续拖拽必卡）。改为「快滑手势 → 整页动画切换」，重绘有界。 */
    lv_obj_clear_flag(tv, LV_OBJ_FLAG_SCROLLABLE);

    /* ={0} 必须：ui_dots_create 会 delete 数组里的旧指针，
     * 未初始化栈垃圾会被当对象删除 → 堆踩踏（滑动几次后卡死） */
    lv_obj_t *dots[UI_DOTS_MAX] = {0};
    int count = app_manager_count();
    int pages = (count + 3) / 4;
#ifdef ESP_PLATFORM
    s_lazy_n = 0; /* 重建（主题/排布变更）时重置错峰队列 */
    s_lazy_i = 0;
#endif
    s_last_page = -1; /* 页点缓存重置 */


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
            /* 打开判定=按下→抬起位移≤14px（滑动拖拽/长按不触发；
             * CLICKED 在 tileview 横滑时仍会误发，用户实测反馈） */
            lv_obj_add_event_cb(tile_btn, tile_press_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)def->id);
            lv_obj_add_event_cb(tile_btn, tile_release_cb, LV_EVENT_RELEASED, NULL);
            lv_obj_add_event_cb(tile_btn, tile_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);

            const int ICON = 108;
            /* 素材缺位时回退符号占位（assets/icons/ 自备图标体系）。
             * 真机：PNG 编入固件（icon_embed 表，esp_lv_decoder 内存解码
             * ——"A:" 文件协议仅模拟器有）；模拟器：文件存在性探测。 */
            const void *icon_src = NULL;
#ifdef ESP_PLATFORM
            if (def->avatar) {
                const char *slash = strrchr(def->avatar, '/');
                const char *dot = strrchr(def->avatar, '.');
                if (slash && dot && dot > slash) {
                    char key[24];
                    size_t n = (size_t)(dot - slash - 1);
                    if (n > 0 && n < sizeof(key)) {
                        memcpy(key, slash + 1, n);
                        key[n] = '\0';
                        icon_src = ui_icon_find(key);
                    }
                }
            }
#else
            if (def->avatar) {
                FILE *f = fopen(def->avatar + (strncmp(def->avatar, "A:", 2) == 0 ? 2 : 0), "rb");
                if (f) { fclose(f); icon_src = def->avatar; }
            }
#endif
            if (icon_src) {
                lv_obj_t *holder = lv_obj_create(tile_btn);
                lv_obj_set_size(holder, ICON, ICON);
                lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, 10);
                lv_obj_set_style_radius(holder, 26, 0);
                lv_obj_set_style_bg_color(holder, UI_CARD, 0);
                lv_obj_set_style_bg_opa(holder, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(holder, 0, 0);
                lv_obj_set_style_pad_all(holder, 0, 0);
                lv_obj_set_style_clip_corner(holder, true, 0);
                /* 图标容器不加阴影：滑动/转场全屏重绘时阴影 alpha
                 * 混合是主要开销（真机管线教训） */
                lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_t *img = lv_image_create(holder);
                /* 图标 108×108 原生 1:1（不缩放、不混合） */
                lv_obj_set_size(img, ICON, ICON);
                lv_obj_center(img);
#ifdef ESP_PLATFORM
                /* 真机：延迟错峰挂载（见 lazy_icon_cb 注释） */
                if (s_lazy_n < LAZY_ICONS_MAX) {
                    s_lazy[s_lazy_n].img = img;
                    s_lazy[s_lazy_n].src = icon_src;
                    s_lazy_n++;
                    if (s_lazy_n == 1) {
                        lv_timer_create(lazy_icon_cb, 500, NULL);
                    }
                }
#else
                lv_image_set_src(img, icon_src);
                /* 模拟器：源 88 → 缩到 108 显示（桌面端无性能压力） */
                lv_image_set_scale(img, (256 * ICON) / 88);
#endif
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
            lv_obj_t *name = ui_label(tile_btn, def->name, &lv_font_cn_16,
                                      def->enabled ? UI_TEXT : UI_TEXT_DIM);
            lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 128);
        }
    }

    /* 页点（tileview 滚动联动）。static 数组会活过 ui_launcher_rebuild
     * 的整屏删除——旧指针已随屏幕释放，必须清零再交给 ui_dots_create
     * （否则对已释放对象再 delete，真机 LoadProhibited 崩溃，2026-11-15） */
    static lv_obj_t *g_page_dots[UI_DOTS_MAX];
    memset(g_page_dots, 0, sizeof(g_page_dots));
    ui_dots_create(g_launcher, g_page_dots, UI_DOTS_MAX, pages, LV_ALIGN_BOTTOM_MID, -8);
    ui_dots_update(g_page_dots, pages, 0); /* 初始高亮（SCROLL 事件不补发） */
    lv_obj_add_event_cb(tv, tileview_scroll_cb, LV_EVENT_SCROLL, g_page_dots);

    if (!s_swipe_timer) s_swipe_timer = lv_timer_create(launcher_swipe_timer_cb, 30, NULL);

    /* 时钟定时器：rebuild 时先删旧（timer 不随屏幕对象释放） */
    static lv_timer_t *clock_timer;
    if (clock_timer) lv_timer_delete(clock_timer);
    clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);

    /* GADGET_DEMO_PAGE=n：启动即翻到第 n 页（headless 页点回归/截图） */
    const char *dpg = getenv("GADGET_DEMO_PAGE");
    if (dpg && dpg[0]) {
        int pi = atoi(dpg);
        if (pi > 0) lv_tileview_set_tile_by_index(tv, pi, 0, LV_ANIM_OFF);
    }

#ifdef ESP_PLATFORM
    (void)0;
#endif
}

static void tileview_scroll_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t **dots = (lv_obj_t **)lv_event_get_user_data(e);
    int pages = (app_manager_count() + 3) / 4;
    int cur = ui_page_of_scroll(tv, 0); /* 步长实测（tv 宽含父级 pad） */
    if (cur < 0) cur = 0;
    if (cur >= pages) cur = pages - 1;
    /* 只在页号变化时改页点：SCROLL 每像素触发，逐帧 set_size 会
     * 全屏重排（9 张图标重绘）→ 显示管线堵塞卡死（用户实测） */
    if (cur == s_last_page) return;
    s_last_page = cur;
    ui_dots_update(dots, pages, cur);
}

/* 打开判定：按下→抬起位移 ≤14px 且 ≤700ms（滑动/拖拽不打开）。
 * 原用 LV_EVENT_CLICKED：tileview 横滑后仍会误发（用户实测）。 */
static struct {
    lv_obj_t *obj;
    lv_point_t start;
    uint32_t t0;
    int app_id;
    int page;
    bool active;
} s_press;

static void tile_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    s_press.obj = lv_event_get_target(e);
    lv_indev_get_point(indev, &s_press.start);
    s_press.t0 = lv_tick_get();
    s_press.app_id = (int)(uintptr_t)lv_event_get_user_data(e);
    s_press.page = s_last_page;
    s_press.active = true;
}

static void tile_press_lost_cb(lv_event_t *e)
{
    (void)e;
    s_press.active = false; /* 滚动接管：本次按压作废 */
}

static void tile_release_cb(lv_event_t *e)
{
    if (!s_press.active || lv_event_get_target(e) != s_press.obj) return;
    s_press.active = false;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    /* 权威判定：按下期间发生过滚动位移 → 一律不打开。
     * （LVGL 内建 press_moved，滚动接管时置位；三保险：press_moved +
     *  位移阈值 + 页号未变，解决"滑动误开 App"反复反馈） */
    if (lv_indev_get_press_moved(indev)) return;
    if (s_swipe_consumed) { s_swipe_consumed = false; return; } /* 已翻页 */
    if (s_last_page != s_press.page) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - s_press.start.x;
    int dy = p.y - s_press.start.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx <= 10 && dy <= 10 && lv_tick_elaps(s_press.t0) <= 600) {
        ui_open_app(s_press.app_id);
    }
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
