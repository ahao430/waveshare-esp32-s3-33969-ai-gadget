/* ui_photo — 本地相册 UI（网格/全屏/设置 3 视图，ADR-018）。
 *
 * 线程模型（同 ui_reader/ui_files）：扫描（photo_scan/probe）、缩略图
 * 解码（photo_thumb_get）、全屏解码（photo_full_decode）、配置落盘
 * （photo_config_save）全部在 worker 线程（ui_worker_start 16KB 栈）；
 * UI 线程零 IO，worker 产物经 lv_async_call + heap 包回 UI（包带 gen，
 * 代际不匹配丢弃+free）。g_gen 在退出/重扫/重建网格时自增作废在途
 * worker；g_busy 门控全屏加载防重入；g_thumb_busy 标记缩略图链在飞。
 *
 * PSRAM 让位策略（ADR-018）：退出时 photo_cache_clear() 释放核心层
 * thumb LRU + 解码画布；UI 侧大缓冲（items/dims/vis/全屏 480² RGB565
 * /thumb 副本池 ≤24×20KB）也一并 free——但仅在无在途 worker 时执行
 * （g_busy/g_thumb_busy/g_scan_busy 任一为真则保留到下次退出再释放，
 * 避免 free 与在途解码写竞争）。缩略图单元格持有 photo 核心缓存指针
 * 会随 LRU（12 槽）驱逐失效，故 UI 侧按可视位置自建 ≤24 槽副本池，
 * 槽位=可视列表下标；超过池上限的单元格保持占位符（v1 边界）。
 *
 * 交互：网格浏览（分类 chips/缩略图/信息面板）→ 点 cell 进全屏
 * （点分区 prev/next/overlay 显隐，KEY3=下一张，幻灯片自动播放）；
 * 设置页改间隔/缩放/自动播放/背景。BOOT 系统级返回。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "ui.h"
#include "ui_theme.h"
#include "app_manager.h"
#include "ui_gesture.h"
#include "ui_worker.h"
#include "ui_keys.h"
#include "photo.h"
#include "sd_paths.h"

/* ---------------- 主题（照 ui_reader.c RD_* 模式） ---------------- */

#define PH_MAIN  0x7C6CFF
#define PH_SUB   0x38BDF8
#define PH_CARD  0x1A1A28
#define PH_TXT   0xC8D0F0
#define PH_DIM   0x8A92B8

/* 缩略图 UI 副本池上限（槽位=可视列表下标；100² RGB565 = 20KB/槽，
 * 24 槽 = 480KB 上限，与核心层 LRU 12 槽解耦防驱逐花屏） */
#define PH_THUMB_POOL 24

/* ---------------- 状态 ---------------- */

static lv_obj_t *g_root, *g_vgrid, *g_vfull, *g_vcfg;
static lv_obj_t *g_hint;                    /* 空态提示（网格视图） */
static lv_obj_t *g_chips;                   /* 分类 chips 行容器（整体重建） */
static lv_obj_t *g_grid;                    /* 缩略图网格滚动容器（整体重建） */
static lv_obj_t *g_idx_lbl;                 /* 底栏 "1/24" */
static lv_obj_t *g_slide_lbl;               /* 底栏幻灯片按钮 label */
static lv_obj_t *g_info_prev, *g_info_prev_ph;   /* 信息面板预览图/占位 */
static lv_obj_t *g_info_name, *g_info_meta, *g_info_time;
static lv_obj_t *g_cfg_rows;                /* 设置项容器（整体重建） */
static lv_obj_t *g_full_img, *g_full_ph;    /* 全屏图/解码失败占位 */
static lv_obj_t *g_full_ovl, *g_full_pill;  /* 全屏 overlay 条/文件名 pill */
static lv_obj_t *g_ovl_idx, *g_ovl_slide_lbl;
static lv_timer_t *g_rtimer;                /* App 级 tick（登记 app_manager） */

static photo_item_t *g_items;               /* 扫描结果（heap，~86KB） */
static int g_items_n;
static photo_cat_t g_cats[PH_CAT_MAX];      /* 分类（≤4，192B 静态可） */
static int g_cats_n;
static int16_t *g_dims;                     /* UI 侧 probe 尺寸 [i*2]=w [i*2+1]=h */
static int16_t *g_vis;                      /* 当前分类可视列表（全局索引） */
static int g_vis_n;
static int g_cat;                           /* 0=全部，1..g_cats_n */
static int g_sel;                           /* 当前选中 item 全局索引 */
static int g_full_sel;                      /* 全屏当前已加载的全局索引（-1=无） */
static int g_view;                          /* 0 网格 1 全屏 2 设置 */
static int g_gen, g_busy;                   /* 代际 + 全屏加载防重入 */
static int g_thumb_busy;                    /* thumb 批量解码链在飞 */
static int g_scan_busy;                     /* 扫描+probe worker 在飞 */
static int g_scan_done;                     /* 配置只load一次的标记 */
static int g_playing;                       /* 幻灯片播放中 */
static int g_auto_left;                     /* 倒计时 tick 数（500ms/tick） */
static int g_ovl_visible;                   /* 全屏 overlay/pill 显隐 */
static int g_save_dirty;                    /* 配置待落盘 */
static photo_config_t g_cfg;

/* thumb 副本池：bufs heap（≤24×20KB，槽位=可视下标），dsc 静态模板 */
static uint16_t *g_tbufs[PH_THUMB_POOL];
static lv_image_dsc_t g_tdsc[PH_THUMB_POOL];
static int16_t g_tslot_item[PH_THUMB_POOL]; /* 槽 s 当前装的 item 全局索引 */
static int g_tpool_n;

static uint16_t *g_full_buf;                /* 全屏解码缓冲 480² RGB565 ≈460KB */
static lv_image_dsc_t g_full_dsc;

/* demo 钩子（headless 截帧回归） */
static int g_demo_scan;                     /* GADGET_DEMO_PHOTO=1（默认行为） */
static int g_demo_open;                     /* GADGET_DEMO_PHOTO_OPEN=N */
static int g_demo_idx;
static int g_demo_view2;                    /* GADGET_DEMO_PHOTO_VIEW=2 */

/* ---------------- 前向声明（跨区引用统一在此） ---------------- */
static void show_view(int v);
static void sel_move(int dir);
static void load_current(void);
static void playing_set(int on);
static void ovl_set_visible(int on);
static void info_refresh(void);
static void sel_borders_refresh(void);
static void chips_rebuild(void);
static void grid_rebuild(void);
static void vis_rebuild(void);
static void thumb_pool_setup(void);
static void thumb_pool_free(void);
static void thumb_fill_start(void);
static void cfg_refresh(void);
static void scan_start(void);
static void init_async(void *arg);
static void *init_worker(void *arg);
static void *thumb_job(void *arg);
static void thumb_async(void *arg);
static void full_async(void *arg);
static void *load_worker(void *arg);
static void *save_worker(void *arg);
static void key3_next(void);
static void demo_hooks(void);

/* ---------------- 小工具 ---------------- */

/* 大小格式化（B/KB/MB 自适应，照 ui_files 口径） */
static void ph_fmt_size(char *out, size_t cap, long v)
{
    if (v >= 1024 * 1024)
        snprintf(out, cap, "%.1f MB", v / 1048576.0);
    else if (v >= 1024)
        snprintf(out, cap, "%.1f KB", v / 1024.0);
    else
        snprintf(out, cap, "%ld B", v);
}

/* mtime → "YYYY-MM-DD HH:MM"（本地时区） */
static void ph_fmt_time(long unix_ts, char *buf, size_t cap)
{
    if (unix_ts <= 0) {
        snprintf(buf, cap, "-");
        return;
    }
    time_t t = (time_t)unix_ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tmv);
}

/* item 绝对路径（sd 相对路径 + TF 根） */
static int ph_abs_of(int idx, char *abs, size_t cap)
{
    if (!g_items || idx < 0 || idx >= g_items_n) return -1;
    return sd_resolve(abs, cap, g_items[idx].rel);
}

/* 全局索引 → 可视列表下标（线性查，≤256） */
static int vis_pos_of(int idx)
{
    for (int k = 0; k < g_vis_n; k++) {
        if (g_vis[k] == idx) return k;
    }
    return -1;
}

/* 视图容器工厂（照 ui_reader make_view） */
static lv_obj_t *ph_make_view(lv_obj_t *scr)
{
    lv_obj_t *v = lv_obj_create(scr);
    lv_obj_set_size(v, 480, 480);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    /* 必须显式清默认主题给普通 lv_obj 的 padding：lv_obj_align 是相对
     * 父容器 content area（含 pad）定位的，不清会把整个视图内容推进
     * (+32,+32)（实测），顶栏撞状态栏、底栏出屏被裁 */
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    return v;
}

/* ---------------- thumb 副本池（槽位=可视列表下标） ---------------- */

static void thumb_pool_setup(void)
{
    int want = g_vis_n < PH_THUMB_POOL ? g_vis_n : PH_THUMB_POOL;
    for (int s = 0; s < PH_THUMB_POOL; s++) {
        if (s < want) {
            if (!g_tbufs[s]) {
                /* 20KB/槽：libc malloc 走 PSRAM（固件
                 * SPIRAM_MALLOC_ALWAYSINTERNAL=2048，坑 #26 口径） */
                g_tbufs[s] = malloc((size_t)PH_THUMB_PX * PH_THUMB_PX * 2);
                if (g_tbufs[s]) {
                    memset(&g_tdsc[s], 0, sizeof(g_tdsc[s]));
                    g_tdsc[s].header.cf = LV_COLOR_FORMAT_RGB565;
                    g_tdsc[s].header.w = PH_THUMB_PX;
                    g_tdsc[s].header.h = PH_THUMB_PX;
                    g_tdsc[s].header.stride = (uint32_t)PH_THUMB_PX * 2;
                    g_tdsc[s].data = (const uint8_t *)g_tbufs[s];
                    g_tdsc[s].data_size = (size_t)PH_THUMB_PX * PH_THUMB_PX * 2;
                }
            }
        } else if (g_tbufs[s]) {
            free(g_tbufs[s]);
            g_tbufs[s] = NULL;
        }
    }
    g_tpool_n = want;
    for (int s = 0; s < PH_THUMB_POOL; s++) g_tslot_item[s] = -1;
}

static void thumb_pool_free(void)
{
    for (int s = 0; s < PH_THUMB_POOL; s++) {
        if (g_tbufs[s]) {
            free(g_tbufs[s]);
            g_tbufs[s] = NULL;
        }
    }
    g_tpool_n = 0;
}

/* ---------------- 可视列表（当前分类过滤，UI 线程纯内存） ---------------- */

static void vis_rebuild(void)
{
    if (!g_vis) {
        g_vis_n = 0;
        return;
    }
    int n = 0;
    for (int i = 0; i < g_items_n && n < PH_LIST_MAX; i++) {
        int c = g_items[i].cat;
        int match = (g_cat == 0) ? 1 : (c >= 0 && c + 1 == g_cat);
        if (match) g_vis[n++] = (int16_t)i;
    }
    g_vis_n = n;
}

/* ---------------- 网格 cell / 边框 / 信息面板 ---------------- */

/* 全局索引 → 网格 cell（user_data 存全局索引） */
static lv_obj_t *cell_of(int idx)
{
    if (!g_grid) return NULL;
    uint32_t n = lv_obj_get_child_count(g_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(g_grid, i);
        if ((int)(intptr_t)lv_obj_get_user_data(c) == idx) return c;
    }
    return NULL;
}

/* cell 填图（清占位 → 92²；LVGL 9.2 无 CONTAIN 档，thumb 本身 100²
 * cover-crop 方形，STRETCH 到 92² 等比无损） */
static void cell_show_thumb(lv_obj_t *cell, int slot)
{
    if (!cell || slot < 0 || slot >= PH_THUMB_POOL || !g_tbufs[slot]) return;
    while (lv_obj_get_child_count(cell) > 0)
        lv_obj_delete(lv_obj_get_child(cell, 0));
    lv_obj_t *img = lv_image_create(cell);
    lv_obj_set_size(img, 92, 92);
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_src(img, &g_tdsc[slot]);
}

/* 选中边框轻量更新（遍历已建 cell 比对 user_data；不重建网格） */
static void sel_borders_refresh(void)
{
    if (!g_grid) return;
    uint32_t n = lv_obj_get_child_count(g_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(g_grid, i);
        int idx = (int)(intptr_t)lv_obj_get_user_data(c);
        int on = (idx == g_sel);
        lv_obj_set_style_border_width(c, on ? 2 : 0, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(PH_SUB), 0);
        if (on) lv_obj_scroll_to_view(c, LV_ANIM_OFF);
    }
}

/* 信息面板刷新（名称/分辨率·大小/时间/预览） */
static void info_refresh(void)
{
    if (!g_info_name) return;
    if (!g_items || g_items_n <= 0 || g_sel < 0 || g_sel >= g_items_n) {
        lv_label_set_text(g_info_name, "-");
        lv_label_set_text(g_info_meta, "-");
        lv_label_set_text(g_info_time, "-");
        if (g_info_prev) lv_obj_add_flag(g_info_prev, LV_OBJ_FLAG_HIDDEN);
        if (g_info_prev_ph)
            lv_obj_clear_flag(g_info_prev_ph, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const photo_item_t *it = &g_items[g_sel];
    lv_label_set_text(g_info_name, it->name);
    int w = g_dims ? g_dims[g_sel * 2] : -1;
    int h = g_dims ? g_dims[g_sel * 2 + 1] : -1;
    char buf[96], sz[24];
    ph_fmt_size(sz, sizeof(sz), it->size);
    if (w > 0 && h > 0)
        snprintf(buf, sizeof(buf), "%d × %d · %s", w, h, sz);
    else
        snprintf(buf, sizeof(buf), "%s", sz);
    lv_label_set_text(g_info_meta, buf);
    ph_fmt_time(it->mtime, buf, sizeof(buf));
    lv_label_set_text(g_info_time, buf);
    /* 预览：池里已解码则显示，否则符号占位 */
    int slot = -1;
    for (int s = 0; s < g_tpool_n; s++) {
        if (g_tslot_item[s] == g_sel) {
            slot = s;
            break;
        }
    }
    if (slot >= 0 && g_info_prev) {
        lv_obj_clear_flag(g_info_prev, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_inner_align(g_info_prev, LV_IMAGE_ALIGN_STRETCH);
        lv_image_set_src(g_info_prev, &g_tdsc[slot]);
        if (g_info_prev_ph)
            lv_obj_add_flag(g_info_prev_ph, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (g_info_prev) lv_obj_add_flag(g_info_prev, LV_OBJ_FLAG_HIDDEN);
        if (g_info_prev_ph)
            lv_obj_clear_flag(g_info_prev_ph, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------- 通用按钮工厂 ---------------- */

static lv_obj_t *ph_btn(lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
                        int32_t h, const char *txt, const lv_font_t *f,
                        uint32_t color, lv_event_cb_t cb, int ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(PH_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)ud);
    lv_obj_t *l = ui_label(b, txt, f, lv_color_hex(color));
    lv_obj_center(l);
    return b;
}

/* 底栏/overlay 的 "i/N" 标签统一刷新 */
static void idx_labels_update(void)
{
    char buf[24];
    int pos = vis_pos_of(g_sel);
    if (g_vis_n > 0 && pos >= 0)
        snprintf(buf, sizeof(buf), "%d/%d", pos + 1, g_vis_n);
    else
        snprintf(buf, sizeof(buf), "-/-");
    if (g_idx_lbl) lv_label_set_text(g_idx_lbl, buf);
    if (g_ovl_idx) lv_label_set_text(g_ovl_idx, buf);
}

/* ---------------- 网格回调 ---------------- */

static void cell_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_items_n) return;
    g_sel = idx;
    sel_borders_refresh();
    info_refresh();
    show_view(1);
}

static void chip_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > g_cats_n) return;
    g_cat = i;
    chips_rebuild();
    grid_rebuild();
}

static void back_home_cb(lv_event_t *e)
{
    (void)e;
    app_manager_back();
}

static void sel_prev_cb(lv_event_t *e)
{
    (void)e;
    sel_move(-1); /* 网格态：只移选中，不进全屏 */
}

static void sel_next_cb(lv_event_t *e)
{
    (void)e;
    sel_move(+1);
}

static void slide_cb(lv_event_t *e)
{
    (void)e;
    playing_set(!g_playing);
}

static void gear_cb(lv_event_t *e)
{
    (void)e;
    show_view(2);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    if (g_scan_busy) return;
    g_gen++;          /* 作废在途 worker */
    g_thumb_busy = 0;
    scan_start();
}

/* ---------------- chips 行（整体重建） ---------------- */

static void chips_rebuild(void)
{
    if (!g_chips) return;
    lv_obj_clean(g_chips);
    int n = g_cats_n + 1;
    if (n > PH_CAT_MAX + 1) n = PH_CAT_MAX + 1;
    for (int i = 0; i < n; i++) {
        char txt[48];
        if (i == 0)
            snprintf(txt, sizeof(txt), "全部 %d", g_items_n);
        else
            snprintf(txt, sizeof(txt), "%.30s %d", g_cats[i - 1].name,
                     g_cats[i - 1].count);
        lv_obj_t *c = lv_obj_create(g_chips);
        lv_obj_set_size(c, 108, 32);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, i * 116, 0);
        int on = (g_cat == i);
        lv_obj_set_style_bg_color(c, lv_color_hex(on ? PH_MAIN : PH_CARD), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 16, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(c, txt, &lv_font_cn_12,
                               lv_color_hex(on ? 0xFFFFFF : PH_DIM));
        lv_obj_set_width(l, 100);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_center(l);
    }
}

/* ---------------- 网格重建（cells + 占位 + 边框 + 链启动） ---------------- */

static void grid_rebuild(void)
{
    if (!g_grid) return;
    g_gen++;             /* 重建 = 代际自增，旧 thumb 链作废 */
    g_thumb_busy = 0;
    vis_rebuild();
    thumb_pool_setup();
    lv_obj_clean(g_grid);
    for (int k = 0; k < g_vis_n; k++) {
        int idx = g_vis[k];
        lv_obj_t *cell = lv_obj_create(g_grid);
        lv_obj_set_size(cell, 100, 100);
        lv_obj_set_user_data(cell, (void *)(intptr_t)idx);
        lv_obj_set_style_bg_color(cell, lv_color_hex(PH_CARD), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 12, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, cell_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)idx);
        lv_obj_t *ph = ui_label(cell, LV_SYMBOL_IMAGE, &lv_font_montserrat_16,
                                lv_color_hex(PH_DIM));
        lv_obj_center(ph);
    }
    sel_borders_refresh();
    info_refresh();
    idx_labels_update();
    thumb_fill_start();
}

/* ---------------- 网格视图组装（顶栏/chips/网格/信息面板/底栏） ---------------- */

static void build_grid(lv_obj_t *parent)
{
    /* 顶栏：图标 + 标题 + 刷新 */
    lv_obj_t *ic = ui_label(parent, LV_SYMBOL_IMAGE, &lv_font_cn_20,
                            lv_color_hex(PH_SUB));
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 20, 42);
    lv_obj_t *tt = ui_label(parent, "图片", &lv_font_cn_20,
                            lv_color_hex(0xFFFFFF));
    lv_obj_align(tt, LV_ALIGN_TOP_LEFT, 52, 40);
    lv_obj_t *rb = ph_btn(parent, 364, 40, 96, 34, "刷新", &lv_font_cn_14,
                          PH_TXT, refresh_cb, 0);
    lv_obj_set_style_border_color(rb, UI_LINE, 0);
    lv_obj_set_style_border_width(rb, 1, 0);

    /* chips 行容器（内容整体重建） */
    g_chips = lv_obj_create(parent);
    lv_obj_set_size(g_chips, 456, 32);
    lv_obj_align(g_chips, LV_ALIGN_TOP_LEFT, 12, 76);
    lv_obj_set_style_bg_opa(g_chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_chips, 0, 0);
    lv_obj_set_style_pad_all(g_chips, 0, 0);
    lv_obj_clear_flag(g_chips, LV_OBJ_FLAG_SCROLLABLE);

    /* 网格滚动容器（cells 重建式更新） */
    g_grid = lv_obj_create(parent);
    lv_obj_set_size(g_grid, 480, 216);
    lv_obj_align(g_grid, LV_ALIGN_TOP_LEFT, 0, 108);
    lv_obj_set_style_bg_opa(g_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_grid, 0, 0);
    lv_obj_set_style_pad_left(g_grid, 16, 0);
    lv_obj_set_style_pad_right(g_grid, 8, 0);
    lv_obj_set_style_pad_top(g_grid, 4, 0);
    lv_obj_set_style_pad_bottom(g_grid, 4, 0);
    lv_obj_set_style_pad_column(g_grid, 8, 0);
    lv_obj_set_style_pad_row(g_grid, 8, 0);
    lv_obj_set_flex_flow(g_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(g_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_grid, LV_SCROLLBAR_MODE_OFF);

    /* 信息面板 */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 456, 96);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 12, 330);
    lv_obj_set_style_bg_color(card, lv_color_hex(PH_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    g_info_prev = lv_image_create(card);
    lv_obj_set_size(g_info_prev, 84, 84);
    lv_obj_align(g_info_prev, LV_ALIGN_LEFT_MID, 10, 0);
    lv_image_set_inner_align(g_info_prev, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(g_info_prev, LV_OBJ_FLAG_HIDDEN);
    g_info_prev_ph = ui_label(card, LV_SYMBOL_IMAGE, &lv_font_montserrat_16,
                              lv_color_hex(PH_DIM));
    lv_obj_align(g_info_prev_ph, LV_ALIGN_LEFT_MID, 10, 0);

    g_info_name = ui_label(card, "-", &lv_font_cn_14, lv_color_hex(0xFFFFFF));
    lv_obj_set_width(g_info_name, 330);
    lv_label_set_long_mode(g_info_name, LV_LABEL_LONG_DOT);
    lv_obj_align(g_info_name, LV_ALIGN_TOP_LEFT, 112, 10);
    g_info_meta = ui_label(card, "-", &lv_font_cn_12, lv_color_hex(PH_DIM));
    lv_obj_set_width(g_info_meta, 330);
    lv_label_set_long_mode(g_info_meta, LV_LABEL_LONG_DOT);
    lv_obj_align(g_info_meta, LV_ALIGN_TOP_LEFT, 112, 36);
    g_info_time = ui_label(card, "-", &lv_font_cn_12, lv_color_hex(PH_DIM));
    lv_obj_set_width(g_info_time, 330);
    lv_label_set_long_mode(g_info_time, LV_LABEL_LONG_DOT);
    lv_obj_align(g_info_time, LV_ALIGN_TOP_LEFT, 112, 60);

    /* 空态提示 */
    g_hint = ui_label(parent, "", &lv_font_cn_14, lv_color_hex(PH_DIM));
    lv_obj_align(g_hint, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);

    /* 底栏：返回 | prev | i/N | next | 幻灯片 | 设置 */
    ph_btn(parent, 8, 433, 72, 40, "返回", &lv_font_cn_14, PH_TXT,
           back_home_cb, 0);
    ph_btn(parent, 92, 433, 40, 40, LV_SYMBOL_LEFT, &lv_font_montserrat_16,
           PH_TXT, sel_prev_cb, 0);
    g_idx_lbl = ui_label(parent, "-/-", &lv_font_cn_14, lv_color_hex(PH_TXT));
    lv_obj_set_width(g_idx_lbl, 64);
    lv_obj_set_style_text_align(g_idx_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_idx_lbl, LV_ALIGN_TOP_LEFT, 144, 443);
    ph_btn(parent, 220, 433, 40, 40, LV_SYMBOL_RIGHT, &lv_font_montserrat_16,
           PH_TXT, sel_next_cb, 0);
    lv_obj_t *sb = ph_btn(parent, 272, 433, 96, 40,
                          LV_SYMBOL_PLAY " 幻灯片", &lv_font_cn_12, PH_TXT,
                          slide_cb, 0);
    g_slide_lbl = lv_obj_get_child(sb, 0);
    ph_btn(parent, 380, 433, 80, 40, "设置", &lv_font_cn_14, PH_TXT,
           gear_cb, 0);
}

/* ---------------- thumb 批量解码链（worker → async → 下一棒） ---------------- */

/* 链请求包（heap）：从可视列表 pos 起解一批 */
typedef struct {
    int gen;
    int pos;
} ph_thumb_req_t;

/* 链结果包（heap）：≤4 张结果 + 是否链尾 */
typedef struct {
    int gen;
    int pos;
    int n;
    int last;
    struct {
        int idx;                /* item 全局索引 */
        int slot;               /* 池槽位（=可视下标） */
        int ok;
    } it[4];
} ph_thumb_res_t;

/* UI 线程启动链（网格重建后调用；gen 已随重建自增） */
static void thumb_fill_start(void)
{
    if (g_thumb_busy || !g_vis || g_vis_n <= 0) return;
    g_thumb_busy = 1;
    ph_thumb_req_t *q = malloc(sizeof(*q));
    if (!q) {
        g_thumb_busy = 0;
        return;
    }
    q->gen = g_gen;
    q->pos = 0;
    if (ui_worker_start(thumb_job, q, 16 * 1024) != 0) {
        free(q);
        g_thumb_busy = 0;
    }
}

/* worker：解一批（≤4 张），每张解码后复查 gen；链尾在 async 决定 */
static void *thumb_job(void *arg)
{
    ph_thumb_req_t *q = arg;
    int gen = q->gen, pos = q->pos;
    free(q);
    ph_thumb_res_t *r = malloc(sizeof(*r));
    if (!r) return NULL;
    r->gen = gen;
    r->pos = pos;
    r->n = 0;
    r->last = 0;
    for (int k = 0; k < 4; k++) {
        int vp = pos + k;
        if (vp >= g_vis_n || vp >= PH_THUMB_POOL) {
            r->last = 1;
            break;
        }
        if (gen != g_gen) {          /* 代际失效：静默丢弃 */
            free(r);
            return NULL;
        }
        int idx = g_vis[vp];
        char abs[PH_PATH_MAX + 64];
        int ok = 0;
        if (ph_abs_of(idx, abs, sizeof(abs)) == 0) {
            const uint16_t *t = photo_thumb_get(abs);
            if (gen != g_gen) {      /* 阻塞调用后复查 */
                free(r);
                return NULL;
            }
            if (t && g_tbufs[vp]) {
                memcpy(g_tbufs[vp], t, (size_t)PH_THUMB_PX * PH_THUMB_PX * 2);
                ok = 1;
            }
        }
        r->it[r->n].idx = idx;
        r->it[r->n].slot = vp;
        r->it[r->n].ok = ok;
        r->n++;
    }
    if (pos + 4 >= g_vis_n || pos + 4 >= PH_THUMB_POOL) r->last = 1;
    if (r->gen == g_gen) {
        lv_async_call(thumb_async, r);
        return NULL;
    }
    free(r);
    return NULL;
}

/* async：填 cell 图 + 记槽位 → 续链 / 收链 */
static void thumb_async(void *arg)
{
    ph_thumb_res_t *r = arg;
    int gen = r->gen, pos = r->pos, n = r->n, last = r->last;
    /* 先拷贝结果再 free 包 */
    struct {
        int idx, slot, ok;
    } it[4];
    for (int i = 0; i < n; i++) {
        it[i].idx = r->it[i].idx;
        it[i].slot = r->it[i].slot;
        it[i].ok = r->it[i].ok;
    }
    free(r);
    if (!g_root || gen != g_gen) {
        g_thumb_busy = 0;            /* 代际失效：链终止 */
        return;
    }
    int sel_touched = 0;
    for (int i = 0; i < n; i++) {
        if (!it[i].ok) continue;
        g_tslot_item[it[i].slot] = (int16_t)it[i].idx;
        lv_obj_t *cell = cell_of(it[i].idx);
        if (cell) cell_show_thumb(cell, it[i].slot);
        if (it[i].idx == g_sel) sel_touched = 1;
    }
    if (sel_touched) info_refresh();
    if (last) {
        g_thumb_busy = 0;
        return;
    }
    ph_thumb_req_t *q = malloc(sizeof(*q));
    if (!q) {
        g_thumb_busy = 0;
        return;
    }
    q->gen = gen;
    q->pos = pos + 4;
    if (ui_worker_start(thumb_job, q, 16 * 1024) != 0) {
        free(q);
        g_thumb_busy = 0;
    }
}

/* ---------------- 选中移动 / 播放 / overlay 显隐 ---------------- */

/* 移动选中（wrap），更新边框/信息/idx；不触发解码（照规格 prev/next
 * 在网格态只移选中，全屏态由调用方再 load_current） */
static void sel_move(int dir)
{
    if (!g_items || g_items_n <= 0 || g_vis_n <= 0 || !g_vis) return;
    int pos = vis_pos_of(g_sel);
    if (pos < 0) pos = 0;
    pos = (pos + dir + g_vis_n) % g_vis_n;
    g_sel = g_vis[pos];
    sel_borders_refresh();
    info_refresh();
    idx_labels_update();
}

static void playing_set(int on)
{
    if (on && (g_vis_n <= 0 || !g_root)) return;
    g_playing = on ? 1 : 0;
    if (on) g_auto_left = g_cfg.interval_sec * 2;
    const char *txt = on ? (LV_SYMBOL_PAUSE " 幻灯片")
                         : (LV_SYMBOL_PLAY " 幻灯片");
    if (g_slide_lbl) lv_label_set_text(g_slide_lbl, txt);
    if (g_ovl_slide_lbl) lv_label_set_text(g_ovl_slide_lbl, txt);
}

static void ovl_set_visible(int on)
{
    g_ovl_visible = on ? 1 : 0;
    if (g_full_ovl) {
        if (on)
            lv_obj_clear_flag(g_full_ovl, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_full_ovl, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_full_pill) {
        if (on)
            lv_obj_clear_flag(g_full_pill, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_full_pill, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------- 全屏回调 ---------------- */

static void back_grid_cb(lv_event_t *e)
{
    (void)e;
    show_view(0);
}

static void ov_prev_cb(lv_event_t *e)
{
    (void)e;
    sel_move(-1);
    load_current();
}

static void ov_next_cb(lv_event_t *e)
{
    (void)e;
    sel_move(+1);
    load_current();
}

static void ovl_toggle(void)
{
    ovl_set_visible(!g_ovl_visible);
}

/* 点分区：左 1/3 prev / 右 1/3 next / 中央 overlay+pill 显隐 */
static void full_click_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(NULL, &p);
    if (p.x < 160) {
        sel_move(-1);
        load_current();
    } else if (p.x > 320) {
        sel_move(+1);
        load_current();
    } else {
        ovl_toggle();
    }
}

/* ---------------- 全屏视图组装 ---------------- */

static void build_full(lv_obj_t *parent)
{
    /* 背景色在 show_view(1) 按 cfg.bg_dark 刷新 */
    g_full_img = lv_image_create(parent);
    lv_obj_set_size(g_full_img, 480, 480);
    lv_obj_align(g_full_img, LV_ALIGN_CENTER, 0, 0);
    /* 解码层已按 zoom_fill 输出 ≤480²，CENTER 原尺寸居中即"适应" */
    lv_image_set_inner_align(g_full_img, LV_IMAGE_ALIGN_CENTER);
    g_full_ph = ui_label(parent, LV_SYMBOL_IMAGE, &lv_font_montserrat_16,
                         lv_color_hex(PH_DIM));
    lv_obj_center(g_full_ph);
    lv_obj_add_flag(g_full_ph, LV_OBJ_FLAG_HIDDEN);

    /* 点击分区层（先于 overlay 建：overlay 按钮在其上层优先收点） */
    lv_obj_t *click = lv_obj_create(parent);
    lv_obj_remove_style_all(click);
    lv_obj_set_size(click, 480, 480);
    lv_obj_add_flag(click, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(click, full_click_cb, LV_EVENT_CLICKED, NULL);

    /* 底部 overlay 条（y=432 h=48） */
    g_full_ovl = lv_obj_create(parent);
    lv_obj_set_size(g_full_ovl, 480, 48);
    lv_obj_align(g_full_ovl, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_full_ovl, lv_color_hex(0x141424), 0);
    lv_obj_set_style_bg_opa(g_full_ovl, LV_OPA_80, 0);
    lv_obj_set_style_border_width(g_full_ovl, 0, 0);
    lv_obj_set_style_pad_all(g_full_ovl, 0, 0);
    lv_obj_clear_flag(g_full_ovl, LV_OBJ_FLAG_SCROLLABLE);
    ph_btn(g_full_ovl, 68, 6, 64, 36, "返回", &lv_font_cn_12, PH_TXT,
           back_grid_cb, 0);
    ph_btn(g_full_ovl, 144, 6, 40, 36, LV_SYMBOL_LEFT, &lv_font_montserrat_16,
           PH_TXT, ov_prev_cb, 0);
    g_ovl_idx = ui_label(g_full_ovl, "-/-", &lv_font_cn_12,
                         lv_color_hex(PH_TXT));
    lv_obj_set_width(g_ovl_idx, 72);
    lv_obj_set_style_text_align(g_ovl_idx, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_ovl_idx, LV_ALIGN_TOP_LEFT, 196, 14);
    ph_btn(g_full_ovl, 280, 6, 40, 36, LV_SYMBOL_RIGHT, &lv_font_montserrat_16,
           PH_TXT, ov_next_cb, 0);
    lv_obj_t *sb = ph_btn(g_full_ovl, 332, 6, 80, 36, LV_SYMBOL_PLAY,
                          &lv_font_cn_12, PH_TXT, slide_cb, 0);
    g_ovl_slide_lbl = lv_obj_get_child(sb, 0);

    /* 文件名 pill（顶部居中） */
    g_full_pill = lv_obj_create(parent);
    lv_obj_set_size(g_full_pill, 300, 30);
    lv_obj_align(g_full_pill, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(g_full_pill, lv_color_hex(0x141424), 0);
    lv_obj_set_style_bg_opa(g_full_pill, LV_OPA_80, 0);
    lv_obj_set_style_radius(g_full_pill, 15, 0);
    lv_obj_set_style_border_width(g_full_pill, 0, 0);
    lv_obj_set_style_pad_all(g_full_pill, 0, 0);
    lv_obj_clear_flag(g_full_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *pl = ui_label(g_full_pill, "", &lv_font_cn_12,
                            lv_color_hex(PH_TXT));
    lv_obj_set_width(pl, 284);
    lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(pl);
}

/* ---------------- 全屏加载（worker 解码 → async 应用） ---------------- */

/* 加载结果包（heap）：sel/成功/EXIF 旋转/输出尺寸 */
typedef struct {
    int gen, sel, ok, rot, ow, oh;
} ph_load_res_t;

/* UI 线程入口：busy 门控防重入 */
static void load_current(void)
{
    if (g_busy || !g_root || !g_items || g_items_n <= 0) return;
    if (g_sel < 0 || g_sel >= g_items_n || !g_full_buf) return;
    g_busy = 1;
    ph_load_res_t *p = malloc(sizeof(*p));
    if (!p) {
        g_busy = 0;
        return;
    }
    p->gen = g_gen;
    p->sel = g_sel;
    p->ok = 0;
    p->rot = 0;
    p->ow = 0;
    p->oh = 0;
    if (ui_worker_start(load_worker, p, 16 * 1024) != 0) {
        free(p);
        g_busy = 0;
    }
}

/* worker：EXIF 方向 + 全屏解码（单缓冲，busy 门控交接给 async） */
static void *load_worker(void *arg)
{
    ph_load_res_t *p = arg;
    int gen = p->gen;
    if (gen == g_gen && g_items && g_full_buf && p->sel >= 0 &&
        p->sel < g_items_n) {
        char abs[PH_PATH_MAX + 64];
        if (ph_abs_of(p->sel, abs, sizeof(abs)) == 0) {
            int o = photo_exif_orientation(abs);
            p->rot = photo_rot_from_exif(o);
            int ow = 0, oh = 0;
            if (photo_full_decode(abs, g_cfg.zoom_fill, g_full_buf, &ow,
                                  &oh) == 0) {
                p->ok = 1;
                p->ow = ow;
                p->oh = oh;
            }
        }
    }
    if (gen == g_gen) {
        lv_async_call(full_async, p);
        return NULL;
    }
    free(p);
    g_busy = 0;
    return NULL;
}

/* async：应用解码结果（dsc/pivot/旋转）+ pill/idx/边框/信息刷新 */
static void full_async(void *arg)
{
    ph_load_res_t *p = arg;
    int gen = p->gen, sel = p->sel, ok = p->ok, rot = p->rot;
    int ow = p->ow, oh = p->oh;
    free(p);
    g_busy = 0;
    if (!g_root || gen != g_gen) return;
    if (g_sel != sel) {
        /* 解码期间选中已变（快速点击/KEY3）：丢弃本帧，追最新选中 */
        load_current();
        return;
    }
    g_full_sel = sel;
    if (ok && g_full_buf && ow > 0 && oh > 0) {
        memset(&g_full_dsc, 0, sizeof(g_full_dsc));
        g_full_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        g_full_dsc.header.w = (int32_t)ow;
        g_full_dsc.header.h = (int32_t)oh;
        g_full_dsc.header.stride = (uint32_t)ow * 2;
        g_full_dsc.data = (const uint8_t *)g_full_buf;
        g_full_dsc.data_size = (size_t)ow * (size_t)oh * 2;
        if (g_full_img) {
            lv_image_set_src(g_full_img, &g_full_dsc);
            /* set_src 后按实际输出尺寸设 pivot（旋转绕图心） */
            lv_image_set_pivot(g_full_img, ow / 2, oh / 2);
            lv_image_set_rotation(g_full_img, rot);
        }
        if (g_full_ph) lv_obj_add_flag(g_full_ph, LV_OBJ_FLAG_HIDDEN);
    } else if (g_full_ph) {
        lv_obj_clear_flag(g_full_ph, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_full_pill && g_items && sel >= 0 && sel < g_items_n) {
        lv_obj_t *pl = lv_obj_get_child(g_full_pill, 0);
        if (pl) lv_label_set_text(pl, g_items[sel].name);
    }
    sel_borders_refresh();
    info_refresh();
    idx_labels_update();
    if (g_playing) g_auto_left = g_cfg.interval_sec * 2;
}

/* ---------------- 设置视图（照 ui_reader cfg_row 模式） ---------------- */

static const int k_ivals[] = { 3, 5, 10, 15, 30 };

/* 统一回调：user_data = field*10+value。改配置 → 标脏 → 重建高亮 */
static void cfg_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int field = v / 10, val = v % 10;
    if (field == 0) {
        if (val >= 0 && val < 5) g_cfg.interval_sec = k_ivals[val];
        if (g_playing) g_auto_left = g_cfg.interval_sec * 2;
    } else if (field == 1) {
        g_cfg.zoom_fill = val;
        g_full_sel = -1;             /* 下次进全屏按新缩放重解码 */
    } else if (field == 2) {
        g_cfg.auto_play = val;
    } else if (field == 3) {
        g_cfg.bg_dark = val;
        if (g_vfull) {
            lv_obj_set_style_bg_color(
                g_vfull,
                lv_color_hex(g_cfg.bg_dark ? 0x000000 : 0x0D0D1A), 0);
        }
    }
    g_save_dirty = 1;
    cfg_refresh();
}

/* 设置行工厂：label + N 个选项按钮（右对齐），当前值主色高亮 */
static lv_obj_t *cfg_row(lv_obj_t *parent, const char *name, int field,
                         int n_opts, const char *const *opts, int cur,
                         int32_t bw)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(row, lv_color_hex(PH_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lb = ui_label(row, name, &lv_font_cn_14, lv_color_hex(PH_TXT));
    lv_obj_align(lb, LV_ALIGN_LEFT_MID, 14, 0);
    /* 右对齐选项组（LEFT_MID 锚点 + 从右缘反推，避免 RIGHT_MID 偏移歧义） */
    int32_t total = n_opts * bw + (n_opts - 1) * 8;
    for (int i = 0; i < n_opts; i++) {
        lv_obj_t *b = lv_obj_create(row);
        lv_obj_set_size(b, bw, 30);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 440 - 14 - total + i * (bw + 8), 0);
        int on = (i == cur);
        lv_obj_set_style_bg_color(b,
                                  lv_color_hex(on ? PH_MAIN : 0x20203A), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 15, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cfg_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(field * 10 + i));
        lv_obj_t *t = ui_label(b, opts[i], &lv_font_cn_12,
                               lv_color_hex(on ? 0xFFFFFF : PH_DIM));
        lv_obj_center(t);
    }
    return row;
}

/* 设置项重建（进视图/改值时刷新当前高亮） */
static void cfg_refresh(void)
{
    if (!g_cfg_rows) return;
    lv_obj_clean(g_cfg_rows);
    static const char *ivals[] = { "3s", "5s", "10s", "15s", "30s" };
    static const char *zoom[] = { "适应", "填充" };
    static const char *autoo[] = { "关", "开" };
    static const char *bgs[] = { "深蓝", "纯黑" };
    int icur = 1;
    for (int i = 0; i < 5; i++) {
        if (g_cfg.interval_sec == k_ivals[i]) {
            icur = i;
            break;
        }
    }
    cfg_row(g_cfg_rows, "幻灯片间隔", 0, 5, ivals, icur, 54);
    cfg_row(g_cfg_rows, "缩放模式", 1, 2, zoom, g_cfg.zoom_fill ? 1 : 0, 64);
    cfg_row(g_cfg_rows, "自动播放", 2, 2, autoo, g_cfg.auto_play ? 1 : 0, 64);
    cfg_row(g_cfg_rows, "全屏背景", 3, 2, bgs, g_cfg.bg_dark ? 1 : 0, 64);
}

/* 设置视图组装 */
static void build_cfg_page(lv_obj_t *parent)
{
    lv_obj_t *back = lv_obj_create(parent);
    lv_obj_set_size(back, 108, 36);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(PH_CARD), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_grid_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, LV_SYMBOL_LEFT " 返回", &lv_font_cn_14,
                            lv_color_hex(PH_DIM));
    lv_obj_center(bl);

    lv_obj_t *tt = ui_label(parent, "设置", &lv_font_cn_20,
                            lv_color_hex(0xFFFFFF));
    lv_obj_align(tt, LV_ALIGN_TOP_MID, 0, 12);

    g_cfg_rows = lv_obj_create(parent);
    lv_obj_set_size(g_cfg_rows, 440, 220);
    lv_obj_align(g_cfg_rows, LV_ALIGN_TOP_LEFT, 20, 64);
    lv_obj_set_style_bg_opa(g_cfg_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_cfg_rows, 0, 0);
    lv_obj_set_style_pad_all(g_cfg_rows, 0, 0);
    lv_obj_set_style_pad_row(g_cfg_rows, 8, 0);
    lv_obj_set_flex_flow(g_cfg_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_cfg_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_cfg_rows, LV_SCROLLBAR_MODE_OFF);
    cfg_refresh();
}

/* ---------------- show_view（0 网格 1 全屏 2 设置） ---------------- */

static void show_view(int v)
{
    if (!g_root || !g_vgrid || !g_vfull || !g_vcfg) return;
    if (v == 1 && g_items_n <= 0) v = 0;   /* 无图不进全屏 */
    g_view = v;
    lv_obj_add_flag(g_vgrid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_vfull, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_vcfg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *tgt = v == 0   ? g_vgrid
                    : v == 1 ? g_vfull
                             : g_vcfg;
    lv_obj_clear_flag(tgt, LV_OBJ_FLAG_HIDDEN);
    if (v == 0) {
        sel_borders_refresh();
        info_refresh();
        idx_labels_update();
    } else if (v == 1) {
        lv_obj_set_style_bg_color(
            g_vfull, lv_color_hex(g_cfg.bg_dark ? 0x000000 : 0x0D0D1A), 0);
        lv_obj_set_style_bg_opa(g_vfull, LV_OPA_COVER, 0);
        ovl_set_visible(1);
        if (g_full_sel != g_sel && !g_busy) load_current();
        if (g_cfg.auto_play && !g_playing) playing_set(1);
    } else {
        cfg_refresh();
    }
    if (v != 1 && g_playing) playing_set(0);
}

/* ---------------- tick / key3 ---------------- */

static void key3_next(void)
{
    if (g_view != 1) return;      /* KEY3=下一张仅全屏态 */
    sel_move(+1);
    load_current();
}

/* App 级 tick（500ms）：幻灯片倒计时 + 配置 debounce 落盘 */
static void photo_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_root) return;
    if (g_view == 1 && g_playing && !g_busy && g_items_n > 0) {
        if (--g_auto_left <= 0) {
            g_auto_left = g_cfg.interval_sec * 2;
            sel_move(+1);
            load_current();
        }
    }
    static int save_wait;
    if (g_save_dirty && ++save_wait >= 8) {   /* 8×500ms = 4s 脏标记 */
        save_wait = 0;
        g_save_dirty = 0;
        if (ui_worker_start(save_worker, NULL, 12 * 1024) != 0)
            g_save_dirty = 1;
    }
}

/* ---------------- 扫描 / 配置落盘（worker） ---------------- */

/* worker：配置（仅首次）+ 扫描 + probe 尺寸；结果 async 交 UI */
static void *init_worker(void *arg)
{
    int gen = (int)(intptr_t)arg;
    if (!g_scan_done) photo_config_load(&g_cfg);
    if (gen == g_gen && g_items && g_dims) {
        int n = 0, nc = 0;
        photo_scan(g_items, PH_LIST_MAX, &n, g_cats, PH_CAT_MAX, &nc);
        g_items_n = n;
        g_cats_n = nc;
        for (int i = 0; i < n; i++) {
            char abs[PH_PATH_MAX + 64];
            photo_fmt_t fmt;
            int w = -1, h = -1;
            if (ph_abs_of(i, abs, sizeof(abs)) == 0 &&
                photo_probe(abs, &fmt, &w, &h) != 0) {
                w = -1;
                h = -1;
            }
            g_dims[i * 2] = (int16_t)w;
            g_dims[i * 2 + 1] = (int16_t)h;
        }
    }
    /* 无条件回 async（缓冲缺失时按空态收场，避免 g_scan_busy 卡死） */
    if (gen == g_gen)
        lv_async_call(init_async, (void *)(intptr_t)gen);
    return NULL;
}

/* async：扫描结果上屏（chips + 网格 + 空态）+ demo 消费 */
static void init_async(void *arg)
{
    int gen = (int)(intptr_t)arg;
    g_scan_busy = 0;
    if (!g_root || gen != g_gen) return;
    g_scan_done = 1;
    g_sel = 0;
    g_cat = 0;
    g_full_sel = -1;
    if (g_items_n <= 0) {
        if (g_hint) {
            lv_obj_clear_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_hint,
                              "未插卡或无图片\n把图片放进 TF 卡 Pictures/");
        }
    } else if (g_hint) {
        lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    }
    chips_rebuild();
    grid_rebuild();
    if (g_items_n > 0 && g_demo_open) {
        g_demo_open = 0;
        g_sel = (g_demo_idx >= 0 && g_demo_idx < g_items_n) ? g_demo_idx : 0;
        sel_borders_refresh();
        info_refresh();
        show_view(1);
        return;
    }
    if (g_items_n > 0 && g_demo_view2) {
        g_demo_view2 = 0;
        show_view(2);
    }
}

static void scan_start(void)
{
    if (g_scan_busy) return;
    g_scan_busy = 1;
    if (ui_worker_start(init_worker, (void *)(intptr_t)g_gen, 16 * 1024) != 0)
        g_scan_busy = 0;
}

/* 配置落盘 worker（只读静态 g_cfg，与 UI 竞争仅整型写，安全） */
static void *save_worker(void *arg)
{
    (void)arg;
    photo_config_save(&g_cfg);
    return NULL;
}


/* ---------------- build / on_exit ---------------- */

void ui_photo_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    ui_gesture_back_add(scr);
    g_root = scr;
    g_gen++;
    g_view = 0;
    g_busy = 0;
    g_thumb_busy = 0;
    g_scan_busy = 0;
    g_playing = 0;
    g_ovl_visible = 1;
    g_save_dirty = 0;
    g_sel = 0;
    g_cat = 0;
    g_full_sel = -1;
    g_items_n = 0;
    g_cats_n = 0;
    g_vis_n = 0;
    g_tpool_n = 0;
    photo_config_t def = { 5, 0, 0, 0 };   /* interval/zoom/auto/bg */
    g_cfg = def;

    /* 大缓冲只在未分配时 malloc（上次退出若有在途 worker 会保留复用） */
    if (!g_items) g_items = malloc(sizeof(photo_item_t) * PH_LIST_MAX);
    if (!g_dims) g_dims = malloc(sizeof(int16_t) * 2 * PH_LIST_MAX);
    if (!g_vis) g_vis = malloc(sizeof(int16_t) * PH_LIST_MAX);
    if (!g_full_buf)
        g_full_buf = malloc((size_t)PH_FULL_PX * PH_FULL_PX * 2);
    memset(&g_full_dsc, 0, sizeof(g_full_dsc));

    g_vgrid = ph_make_view(scr);
    build_grid(g_vgrid);
    g_vfull = ph_make_view(scr);
    build_full(g_vfull);
    g_vcfg = ph_make_view(scr);
    build_cfg_page(g_vcfg);

    show_view(0);
    ui_key_set_key3_handler(key3_next);
    if (!g_rtimer)
        g_rtimer = app_manager_timer_create(photo_tick_cb, 500, NULL);
    scan_start();
    demo_hooks();
}

void ui_photo_on_exit(void)
{
    g_root = NULL;
    g_vgrid = g_vfull = g_vcfg = NULL;
    g_hint = NULL;
    g_chips = g_grid = NULL;
    g_idx_lbl = g_slide_lbl = NULL;
    g_info_prev = g_info_prev_ph = NULL;
    g_info_name = g_info_meta = g_info_time = NULL;
    g_cfg_rows = NULL;
    g_full_img = g_full_ph = NULL;
    g_full_ovl = g_full_pill = NULL;
    g_ovl_idx = g_ovl_slide_lbl = NULL;
    g_playing = 0;
    /* 退出前进度/配置强落盘（worker，只读 g_cfg 安全） */
    if (g_save_dirty) {
        g_save_dirty = 0;
        ui_worker_start(save_worker, NULL, 12 * 1024);
    }
    g_gen++;                      /* 作废全部在途 worker 的 async 回调 */
    ui_key_set_key3_handler(NULL);
    g_rtimer = NULL;              /* app_manager 登记 timer，切走自动删 */
    /* PSRAM 让位（ADR-018）：无在途 worker 时释放核心解码缓存 + UI 侧
     * 大缓冲；有在途 worker（g_busy/g_thumb_busy/g_scan_busy）时本轮
     * 保留，下次退出空闲再释放——避免 free 与解码写竞争 */
    int busy = (g_busy || g_thumb_busy || g_scan_busy);
    g_busy = g_thumb_busy = g_scan_busy = 0;
    if (!busy) {
        photo_cache_clear();
        thumb_pool_free();
        if (g_items) {
            free(g_items);
            g_items = NULL;
        }
        if (g_dims) {
            free(g_dims);
            g_dims = NULL;
        }
        if (g_vis) {
            free(g_vis);
            g_vis = NULL;
        }
        if (g_full_buf) {
            free(g_full_buf);
            g_full_buf = NULL;
        }
        g_items_n = 0;
        g_cats_n = 0;
        g_vis_n = 0;
        g_full_sel = -1;
        g_scan_done = 0;
    }
}

/* ---------------- 模拟器 demo 钩子（照 ui_reader 模式） ---------------- */

static void demo_hooks(void)
{
    /* GADGET_DEMO_PHOTO=1：基线截帧（scan 完成即网格默认态，无额外动作） */
    if (getenv("GADGET_DEMO_PHOTO")) g_demo_scan = 1;
    /* GADGET_DEMO_PHOTO_OPEN=N（'0'-'9'）：scan 完成自动进全屏第 N 张 */
    const char *op = getenv("GADGET_DEMO_PHOTO_OPEN");
    if (op && op[0] >= '0' && op[0] <= '9') {
        g_demo_idx = atoi(op);
        g_demo_open = 1;
    }
    /* GADGET_DEMO_PHOTO_VIEW=2：scan 完成直接进设置页 */
    const char *vw = getenv("GADGET_DEMO_PHOTO_VIEW");
    if (vw && vw[0] == '2') g_demo_view2 = 1;
}
