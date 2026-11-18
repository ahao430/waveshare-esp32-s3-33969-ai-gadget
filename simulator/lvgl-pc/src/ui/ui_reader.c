/* ui_reader — 小说阅读器 UI（书架/阅读/目录/设置 4 视图，ADR-017）。
 *
 * 线程模型（同 ui_files/ui_music）：书架扫描/开书/翻页渲染全在
 * worker 线程（reader_book 阻塞 IO），lv_async_call 回 UI 线程建
 * label；g_gen 代际计数防悬垂；g_busy 防翻页重入。
 *
 * 阅读页零 chrome：正文 16 行 label 复用（setText），点击分区翻页
 * （左 1/3 上一页/右 1/3 下一页/中央菜单 overlay），KEY3=下一页，
 * BOOT=返回。进度 5s debounce 落 TF。
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include "lvgl.h"
#include "ui.h"
#include "ui_theme.h"
#include "app_manager.h"
#include "ui_gesture.h"
#include "ui_worker.h"
#include "ui_keys.h"
#include "reader_book.h"
#include "sd_paths.h"

extern const lv_font_t lv_font_cn_16;
extern const lv_font_t lv_font_cn_20;
extern const lv_font_t lv_font_cn_24;

/* ---------------- 主题 ---------------- */

#define RD_MAIN  0x7C6CFF
#define RD_SUB   0x38BDF8
#define RD_CARD  0x1A1A28
#define RD_TXT   0xC8D0F0
#define RD_DIM   0x8A92B8

/* 阅读主题 3 档（bg / 正文 / 页脚小字） */
static const uint32_t theme_bg[]  = { 0x0D0D14, 0x000000, 0x1E160E };
static const uint32_t theme_txt[] = { 0xC8D0F0, 0xB8C0D8, 0xD8C0A0 };
static const uint32_t theme_dim[] = { 0x6A72A0, 0x5A6288, 0x8A7A60 };

/* ---------------- 版心与游标 ---------------- */

#define AREA_X   28
#define AREA_W   424
#define AREA_Y   56
#define AREA_H   360
#define FOOT_Y   436

/* ---------------- 状态 ---------------- */

static lv_obj_t *g_root, *g_vshelf, *g_vread, *g_vtoc, *g_vcfg;
static lv_obj_t *g_rows, *g_hint;          /* 书架列表/空态 */
static lv_obj_t *g_recent_card, *g_recent_lbl;
static lv_obj_t *g_txt[RD_MAX_LINES];      /* 正文行 label（复用） */
static lv_obj_t *g_chbar, *g_foot;         /* 章节条/页脚 */
static lv_obj_t *g_toc_rows;               /* 目录列表 */
static lv_obj_t *g_menu;                   /* 菜单 overlay */
static lv_obj_t *g_cfg_rows;               /* 设置项容器 */
static lv_timer_t *g_rtimer;               /* App 级 tick（登记 app_manager） */
static lv_obj_t *g_recent_btn_lbl;

static reader_book_t *g_book;
static reader_page_t *g_page;              /* PSRAM（worker/UI 交接缓冲） */
static reader_shelf_t *g_shelf;            /* 书架缓存（PSRAM） */
static int g_shelf_n;

static int g_cur_ch, g_cur_pg;             /* 游标（UI 侧真值） */
static int g_gen, g_busy;                  /* 代际 + 翻页防抖 */
static int g_shelf_busy;                   /* 书架扫描防重入 */
static int g_view;                         /* 0 书架 1 阅读 2 目录 3 设置 */
static int g_menu_open;
static int g_save_dirty;                   /* 进度待落盘 */
static int g_demo_open;                    /* demo：书架就绪自动开书 */
static int g_demo_idx;                     /* demo 开书序号 */
static char s_open_pending[RD_PATH_MAX];   /* 文件管理器联动（ADR-019） */
static int g_auto_left;                    /* 自动翻页倒计时（tick 计数） */
static reader_config_t g_cfg;
static reader_page_t *g_page_pending;      /* worker 产物交接（双缓冲） */
/* ---------------- 前向声明（跨区引用统一在此） ---------------- */
static void menu_toggle(void);
static void page_next(int dir);
static void page_goto(int ch, int pg);
static void show_view(int v);
static void open_book(int idx);
static void toc_refresh(void);
static void cfg_refresh(void);
static void apply_page_text(void);
static void apply_theme_all(void);
static void shelf_start(void);
static void shelf_async(void *arg);
static void open_fail(void *arg);
static void init_async(void *arg);
static void *page_job(void *arg);
static void *open_worker(void *arg);
static void *save_worker(void *arg);
static void *init_worker(void *arg);
static void *shelf_worker(void *arg);
static void key3_next(void);
static void demo_hooks(void);
static void back_read_cb(lv_event_t *e);
static void gear_cb(lv_event_t *e);

/* <<APPEND>> */

/* ---------------- 翻页状态机（worker 线程执行） ---------------- */

/* 翻页命令包（heap 传递，async 消费后 free） */
typedef struct {
    int cmd;                    /* 0=next 1=prev 2=goto */
    int ch, pg;
    int gen;
} rd_cmd_t;

static void async_show_page(void *arg);

/* worker：执行翻页命令并渲染目标页到 g_page（busy 门控交接） */
static void *page_job(void *arg)
{
    rd_cmd_t *c = arg;
    reader_book_t *b = g_book;
    if (c->gen == g_gen && b && g_page) {
        int ch = g_cur_ch, pg = g_cur_pg;
        int npg;
        if (c->cmd == 2) {
            ch = c->ch;
            pg = c->pg;
        } else if (c->cmd == 0) {
            npg = reader_ch_pages(b, ch, g_cfg.font_px,
                                  reader_gap_px(g_cfg.gap_class,
                                                g_cfg.font_px),
                                  AREA_W, AREA_H);
            if (pg + 1 < npg) {
                pg++;
            } else if (ch + 1 < reader_ch_count(b)) {
                ch++;
                pg = 0;
            }
        } else {
            npg = reader_ch_pages(b, ch, g_cfg.font_px,
                                  reader_gap_px(g_cfg.gap_class,
                                                g_cfg.font_px),
                                  AREA_W, AREA_H);
            if (pg > 0) {
                pg--;
            } else if (ch > 0) {
                ch--;
                npg = reader_ch_pages(b, ch, g_cfg.font_px,
                                      reader_gap_px(g_cfg.gap_class,
                                                    g_cfg.font_px),
                                      AREA_W, AREA_H);
                pg = npg - 1;
            }
        }
        if (pg < 0) pg = 0;
        reader_ch_pages(b, ch, g_cfg.font_px,
                        reader_gap_px(g_cfg.gap_class, g_cfg.font_px),
                        AREA_W, AREA_H);
        long off = reader_page_offset(b, ch, pg);
        if (off < 0) off = reader_ch_offset(b, ch);
        if (reader_render(b, off, ch, g_cfg.font_px,
                          reader_gap_px(g_cfg.gap_class, g_cfg.font_px),
                          AREA_W, AREA_H, g_page) == 0) {
            c->ch = ch;
            c->pg = pg;
            lv_async_call(async_show_page, c);
            return NULL;
        }
    }
    free(c);
    g_busy = 0;
    return NULL;
}

/* 铺正文行 label（UI 线程；g_page 由 busy 门控保证稳定） */
static void apply_page_text(void)
{
    const lv_font_t *f = (g_cfg.font_px <= 16 ? &lv_font_cn_16
                          : g_cfg.font_px <= 20 ? &lv_font_cn_20
                                                : &lv_font_cn_24);
    lv_color_t tc = lv_color_hex(theme_txt[g_cfg.theme]);
    for (int i = 0; i < RD_MAX_LINES; i++) {
        lv_obj_t *L = g_txt[i];
        if (!L) continue;
        int has = i < g_page->nlines && g_page->lines[i][0];
        if (!has) {
            lv_obj_add_flag(L, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(L, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(L, f, 0);
        lv_obj_set_style_text_color(L, tc, 0);
        lv_label_set_text(L, g_page->lines[i]);
    }
}

/* async：游标落定 + 铺页 + 章节条/页脚刷新 */
static void async_show_page(void *arg)
{
    rd_cmd_t *c = arg;
    int ch = c->ch, pg = c->pg, gen = c->gen;
    free(c);
    g_busy = 0;
    if (!g_root || !g_book || gen != g_gen) return;
    g_cur_ch = ch;
    g_cur_pg = pg;
    g_save_dirty = 1;
    g_auto_left = g_cfg.auto_sec;
    apply_page_text();
    char buf[96];
    snprintf(buf, sizeof(buf), "%.56s", reader_ch_title(g_book, ch));
    lv_label_set_text(g_chbar, buf);
    long fsz = reader_fsize(g_book);
    int pct = fsz > 0 ? (int)(g_page->page_off * 100 / fsz) : 0;
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(g_foot, buf);
}

/* UI 翻页入口：投 worker（busy 防重入） */
static void page_next(int dir)
{
    if (g_busy || !g_book || !g_root) return;
    g_busy = 1;
    rd_cmd_t *c = malloc(sizeof(rd_cmd_t));
    if (!c) {
        g_busy = 0;
        return;
    }
    c->cmd = dir > 0 ? 0 : 1;
    c->ch = 0;
    c->pg = 0;
    c->gen = g_gen;
    if (ui_worker_start(page_job, c, 16 * 1024) != 0) {
        free(c);
        g_busy = 0;
    }
}

/* 跳章/续读恢复（cmd=2） */
static void page_goto(int ch, int pg)
{
    if (g_busy || !g_book || !g_root) return;
    g_busy = 1;
    rd_cmd_t *c = malloc(sizeof(rd_cmd_t));
    if (!c) {
        g_busy = 0;
        return;
    }
    c->cmd = 2;
    c->ch = ch;
    c->pg = pg;
    c->gen = g_gen;
    if (ui_worker_start(page_job, c, 16 * 1024) != 0) {
        free(c);
        g_busy = 0;
    }
}

/* ---------------- 阅读视图构建 ---------------- */

static void menu_toggle(void);

/* 阅读页点击分区（背景容器 clicked：左 1/3 上一页/右 1/3 下一页/
 * 中央呼出菜单）。滑动会同时触发 CLICKED，用按下点到释放点距离区分：
 * >24px 视为滑动（翻页由 reader_flick_cb 处理，避免一次滑动翻两页）。 */
static lv_point_t g_press_pt;
static bool g_press_valid;

static void read_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(NULL, &g_press_pt);
    g_press_valid = true;
}

static void read_click_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(NULL, &p);
    if (g_press_valid) {
        g_press_valid = false;
        int dx = p.x - g_press_pt.x;
        int dy = p.y - g_press_pt.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 24 || dy > 24) return; /* 滑动：交给快甩翻页 */
    }
    if (g_menu_open) {
        menu_toggle();
        return;
    }
    if (p.x < 160) page_next(-1);
    else if (p.x > 320) page_next(+1);
    else menu_toggle();
}

/* 阅读视图：章节条 + 16 行正文 label + 页脚 + 主题底色 */
static void build_read(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(theme_bg[g_cfg.theme]), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    g_chbar = ui_label(parent, "", &lv_font_cn_12, lv_color_hex(theme_dim[0]));
    lv_obj_align(g_chbar, LV_ALIGN_TOP_MID, 0, 26);
    for (int i = 0; i < RD_MAX_LINES; i++) {
        lv_obj_t *L = lv_label_create(parent);
        lv_obj_set_width(L, AREA_W);
        lv_label_set_long_mode(L, LV_LABEL_LONG_CLIP);
        lv_obj_align(L, LV_ALIGN_TOP_LEFT, AREA_X,
                     AREA_Y + i * (AREA_H / RD_MAX_LINES));
        lv_obj_set_style_text_color(L, lv_color_hex(theme_txt[0]), 0);
        lv_obj_add_flag(L, LV_OBJ_FLAG_HIDDEN);
        g_txt[i] = L;
    }
    g_foot = ui_label(parent, "", &lv_font_cn_12, lv_color_hex(theme_dim[0]));
    lv_obj_align(g_foot, LV_ALIGN_TOP_MID, 0, FOOT_Y);

    /* 点击层：满屏透明容器收 click（label 不挡） */
    lv_obj_t *click = lv_obj_create(parent);
    lv_obj_remove_style_all(click);
    lv_obj_set_size(click, 480, 480);
    lv_obj_add_flag(click, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(click, read_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(click, read_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------- 菜单 overlay（阅读页中央呼出） ---------------- */

static void menu_toggle(void)
{
    if (!g_menu) return;
    g_menu_open = !g_menu_open;
    if (g_menu_open)
        lv_obj_clear_flag(g_menu, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);
}

static void menu_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    menu_toggle();
    if (act == 0) show_view(2);          /* 目录 */
    else if (act == 1) show_view(3);     /* 设置 */
    else if (act == 2) show_view(0);     /* 返回书架 */
    /* act=-1：仅关闭菜单 */
}

/* 菜单按钮工厂 */
static lv_obj_t *menu_btn(lv_obj_t *parent, const char *txt, int act,
                          int32_t x)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, 120, 64);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, 230);
    ui_style_card(b);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    lv_obj_t *l = ui_label(b, txt, &lv_font_cn_16, lv_color_hex(RD_TXT));
    lv_obj_center(l);
    return b;
}

/* 菜单构建（挂阅读视图顶层；自身 clickable 挡住翻页点击层） */
static void build_menu(lv_obj_t *parent)
{
    g_menu = lv_obj_create(parent);
    lv_obj_remove_style_all(g_menu);
    lv_obj_set_size(g_menu, 480, 480);
    lv_obj_set_style_bg_color(g_menu, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_70, 0);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_menu, menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    menu_btn(g_menu, "目录", 0, -130);
    menu_btn(g_menu, "设置", 1, 0);
    menu_btn(g_menu, "书架", 2, 130);
}

/* ---------------- 书架 ---------------- */

/* 书架容量：用户 book/ 实测 112 本（含三层子目录），64 会截断一半。
 * 256×~420B≈105KB，malloc 大块落 PSRAM（ALWAYSINTERNAL=0 口径），
 * 行对象也在 PSRAM，不占内部 RAM。 */
#define RD_MAX_SHELF 256

/* 大小格式化（KB/MB） */
static const char *fmt_size(long sz)
{
    static char buf[16];
    if (sz >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1fM", sz / 1048576.0);
    else if (sz >= 1024)
        snprintf(buf, sizeof(buf), "%ldK", sz / 1024);
    else
        snprintf(buf, sizeof(buf), "%ldB", sz);
    return buf;
}

static void shelf_retry_async(void *arg)
{
    (void)arg;
    if (g_root) shelf_start();
}

static void *shelf_worker(void *arg)
{
    int gen = (int)(intptr_t)arg;
    if (gen != g_gen) { /* 页面已退出（或已重进）：丢弃并让新页面补扫 */
        g_shelf_busy = 0;
        if (g_root) lv_async_call(shelf_retry_async, NULL);
        return NULL;
    }
    if (!g_shelf) {
        /* 256 × ~420B ≈ 105KB：libc malloc 大块落 PSRAM
         * （固件 SPIRAM_MALLOC_ALWAYSINTERNAL=0，优先 PSRAM） */
        g_shelf = malloc(sizeof(reader_shelf_t) * RD_MAX_SHELF);
    }
    if (g_shelf)
        reader_scan_shelf(g_shelf, RD_MAX_SHELF, &g_shelf_n);
#ifdef ESP_PLATFORM
    ESP_LOGI("reader", "shelf: %d books (cap %d)", g_shelf_n, RD_MAX_SHELF);
#endif
    if (gen == g_gen) {
        lv_async_call(shelf_async, (void *)(intptr_t)gen);
    } else {
        /* 扫描期间页面退出/重进：busy 复位，重进则补扫 */
        g_shelf_busy = 0;
        if (g_root) lv_async_call(shelf_retry_async, NULL);
    }
    return NULL;
}

/* 书架行点击 → 开书 */
static void shelf_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_book(idx);
}

/* async：书架渲染（最近卡 + 行列表） */
static void shelf_async(void *arg)
{
    int gen = (int)(intptr_t)arg;
    g_shelf_busy = 0;
    if (!g_root || gen != g_gen || !g_rows) return;
    lv_obj_clean(g_rows);
    int n = g_shelf_n;
    if (n <= 0 || !g_shelf) {
        if (g_hint) {
            lv_obj_clear_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_hint,
                              g_root ? "TF 卡 book/ 目录没有 TXT 文件\n把小说放进 book/"
                                     : "");
        }
        return;
    }
    if (g_hint) lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < n; i++) {
        const reader_shelf_t *t = &g_shelf[i];
        lv_obj_t *row = lv_obj_create(g_rows);
        lv_obj_set_size(row, LV_PCT(100), 56);
        ui_style_card(row);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, shelf_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *nm = ui_label(row, t->title, &lv_font_cn_14,
                                lv_color_hex(RD_TXT));
        lv_obj_set_width(nm, 300);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, -8);
        char meta[48];
        snprintf(meta, sizeof(meta), "%s · 已读 %d%%",
                 fmt_size(t->size), t->percent);
        lv_obj_t *mt = ui_label(row, meta, &lv_font_cn_12,
                                lv_color_hex(RD_DIM));
        lv_obj_align(mt, LV_ALIGN_LEFT_MID, 14, 10);
        /* 右侧不再重复大小（meta 已含 size · 已读%，112 行省 ~1/4 对象） */
    }
    /* 文件管理器联动：pending 命中书架即开书（ADR-019，优先于 demo） */
    if (s_open_pending[0]) {
        for (int i = 0; i < g_shelf_n; i++) {
            if (strcmp(g_shelf[i].path, s_open_pending) == 0) {
                s_open_pending[0] = 0;
                open_book(i);
                return;
            }
        }
        s_open_pending[0] = 0; /* 未命中（不在 book/ 扫描范围）则丢弃 */
    }
    /* demo：书架就绪后自动开第一本（headless 截帧用） */
    if (g_demo_open) {
        g_demo_open = 0;
        open_book(g_demo_idx);
    }
}

/* 文件管理器联动入口（ADR-019） */
void ui_reader_open_path(const char *rel)
{
    snprintf(s_open_pending, sizeof(s_open_pending), "%s", rel ? rel : "");
}

/* 无触摸自检（GADGET_SELFTEST_READER）：读取当前书架条数 */
int ui_reader_debug_shelf_n(void) { return g_shelf_n; }

/* ---------------- 开书 / 进度保存 ---------------- */

static int g_open_busy;

static void open_async(void *arg)
{
    int gen = (int)(intptr_t)arg;
    g_open_busy = 0;
    if (!g_root || gen != g_gen || !g_book) return;
    g_open_busy = 0;
    show_view(1);
    page_goto(g_cur_ch, g_cur_pg);
}

static void *open_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    int gen = g_gen;
    char rel[RD_PATH_MAX];
    rel[0] = 0;
    if (g_shelf && idx >= 0 && idx < g_shelf_n)
        snprintf(rel, sizeof(rel), "%s", g_shelf[idx].path);
    if (gen != g_gen || !rel[0]) {
        g_open_busy = 0;
        return NULL;
    }
    char abs[RD_PATH_MAX];
    if (sd_resolve(abs, sizeof(abs), rel) != 0) {
        g_open_busy = 0;
        return NULL;
    }
    reader_book_t *b = reader_open(abs, rel);
    if (!b) {
        g_open_busy = 0;
        lv_async_call(open_fail, (void *)(intptr_t)gen);
        return NULL;
    }
    g_book = b;
    reader_progress_get(rel, NULL, &g_cur_ch, &g_cur_pg, NULL);
    lv_async_call(open_async, (void *)(intptr_t)gen);
    return NULL;
}

/* 开书失败提示（async） */
static void open_fail(void *arg)
{
    int gen = (int)(intptr_t)arg;
    g_open_busy = 0;
    if (!g_root || gen != g_gen) return;
    if (g_hint) {
        lv_obj_clear_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_hint,
                          g_root ? "打不开这本书\n仅支持 UTF-8 / GBK 文本"
                                 : "");
    }
}

static void open_book(int idx)
{
    if (g_open_busy || !g_root) return;
    g_open_busy = 1;
    if (ui_worker_start(open_worker, (void *)(intptr_t)idx, 16 * 1024) != 0)
        g_open_busy = 0;
}

/* ---------------- 进度保存（debounce，worker 落盘） ---------------- */

static void *save_worker(void *arg)
{
    (void)arg;
    reader_book_t *b = g_book;
    if (b && g_save_dirty) {
        g_save_dirty = 0;
        reader_progress_set(reader_path(b), g_page->page_off, g_cur_ch,
                        g_cur_pg);
    }
    return NULL;
}

/* 退出即清 RAM（2026-11-18 用户要求）：先进度落盘，再延迟 500ms
 * 等在途渲染 worker 收尾（单页排版百 ms 级），最后释放 book/页缓冲/
 * 书架缓存。zombie 经 heap 交给 worker；worker 起不来退化为同步释放。 */
typedef struct {
    reader_book_t *b;
    reader_page_t *pg;
    reader_shelf_t *sh;
    int dirty;
} rd_zombie_t;

static void rd_zombie_free(rd_zombie_t *z)
{
    if (!z) return;
    if (z->b) reader_close(z->b);
    free(z->pg);
    free(z->sh);
    free(z);
}

static void *reader_teardown_worker(void *arg)
{
    rd_zombie_t *z = (rd_zombie_t *)arg;
    if (z->b && z->dirty && z->pg) {
        reader_progress_set(reader_path(z->b), z->pg->page_off,
                            g_cur_ch, g_cur_pg);
    }
    usleep(500 * 1000);
    rd_zombie_free(z);
    return NULL;
}

/* ---------------- 目录视图 ---------------- */

static void toc_row_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    show_view(1);
    page_goto(ch, 0);
}

/* 目录刷新（show_view(2) 时重建；当前章高亮） */
static void toc_refresh(void)
{
    if (!g_toc_rows || !g_book) return;
    lv_obj_clean(g_toc_rows);
    int n = reader_ch_count(g_book);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(g_toc_rows);
        lv_obj_set_size(row, LV_PCT(100), 44);
        ui_style_card(row);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, toc_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *t = ui_label(row, reader_ch_title(g_book, i),
                               &lv_font_cn_14,
                               lv_color_hex(i == g_cur_ch ? RD_MAIN
                                                          : RD_TXT));
        lv_obj_set_width(t, 360);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_center(t);
    }
}

/* ---------------- 设置视图 ---------------- */

/* 统一回调：user_data = field*10+value。改配置 → 落盘 → 重排当前页 */
static void cfg_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int field = v / 10, val = v % 10;
    if (field == 0)
        g_cfg.font_px = (val == 0) ? 16 : (val == 1) ? 20 : 24;
    else if (field == 1)
        g_cfg.gap_class = val;
    else if (field == 2)
        g_cfg.theme = val;
    else if (field == 3)
        g_cfg.auto_sec = val;
    reader_config_save(&g_cfg);
    if (g_book && g_root) page_goto(g_cur_ch, g_cur_pg);
}

/* 设置行工厂：label + N 个选项按钮，当前值主色高亮 */
static lv_obj_t *cfg_row(lv_obj_t *parent, const char *name, int field,
                         int n_opts, const char *const *opts, int cur)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 64);
    ui_style_card(row);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lb = ui_label(row, name, &lv_font_cn_14,
                            lv_color_hex(RD_TXT));
    lv_obj_align(lb, LV_ALIGN_LEFT_MID, 14, 0);
    int total_w = n_opts * 78 + (n_opts - 1) * 8;
    int x0 = -(total_w / 2) + 39;
    for (int i = 0; i < n_opts; i++) {
        lv_obj_t *b = lv_obj_create(row);
        lv_obj_set_size(b, 78, 44);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, x0 + i * 86, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_color(b,
                                  lv_color_hex(i == cur ? RD_MAIN
                                                        : RD_CARD),
                                  0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b,
                                      lv_color_hex(i == cur ? RD_MAIN
                                                            : 0x2A2A40),
                                      0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cfg_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(field * 10 + i));
        lv_obj_t *t = ui_label(b, opts[i], &lv_font_cn_12,
                               lv_color_hex(i == cur ? 0xFFFFFF : RD_DIM));
        lv_obj_center(t);
    }
    return row;
}

/* 设置视图组装 */
static void build_cfg(lv_obj_t *parent)
{
    lv_obj_t *back = lv_obj_create(parent);
    lv_obj_set_size(back, 108, 36);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_read_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, LV_SYMBOL_LEFT " 返回", &lv_font_cn_14,
                            lv_color_hex(RD_DIM));
    lv_obj_center(bl);

    lv_obj_t *tt = ui_label(parent, "阅读设置", &lv_font_cn_16,
                            lv_color_hex(RD_TXT));
    lv_obj_align(tt, LV_ALIGN_TOP_MID, 0, 12);

    g_cfg_rows = lv_obj_create(parent);
    lv_obj_set_size(g_cfg_rows, 440, 380);
    lv_obj_align(g_cfg_rows, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(g_cfg_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_cfg_rows, 0, 0);
    lv_obj_set_style_pad_all(g_cfg_rows, 0, 0);
    lv_obj_set_flex_flow(g_cfg_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_cfg_rows, 10, 0);
    lv_obj_set_scroll_dir(g_cfg_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_cfg_rows, LV_SCROLLBAR_MODE_OFF);
    cfg_refresh();
}

/* 设置项重建（进视图时刷新当前值高亮） */
static void cfg_refresh(void)
{
    if (!g_cfg_rows) return;
    lv_obj_clean(g_cfg_rows);    static const char *fonts[] = { "小", "中", "大" };
    static const char *gaps[] = { "紧凑", "标准", "宽松" };
    static const char *themes[] = { "霓虹", "纯黑", "护眼" };
    static const char *autos[] = { "关", "5s", "15s", "30s" };
    int auto_idx = g_cfg.auto_sec == 0 ? 0
                   : g_cfg.auto_sec <= 5  ? 1
                   : g_cfg.auto_sec <= 15 ? 2
                                          : 3;
    int font_idx = g_cfg.font_px <= 16 ? 0 : g_cfg.font_px <= 20 ? 1 : 2;
    cfg_row(g_cfg_rows, "字号", 0, 3, fonts, font_idx);
    cfg_row(g_cfg_rows, "行距", 1, 3, gaps, g_cfg.gap_class);
    cfg_row(g_cfg_rows, "主题", 2, 3, themes, g_cfg.theme);
    cfg_row(g_cfg_rows, "自动翻页", 3, 4, autos, auto_idx);
}

/* ---------------- show_view / 书架视图组装 ---------------- */

static void back_read_cb(lv_event_t *e)
{
    (void)e;
    show_view(1);
}

/* 视图切换（0 书架 1 阅读 2 目录 3 设置）；目录/设置进入时刷新 */
static void show_view(int v)
{
    g_view = v;
    if (!g_root) return;
    lv_obj_add_flag(g_vshelf, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_vread, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_vtoc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_vcfg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *tgt = v == 0   ? g_vshelf
                    : v == 1 ? g_vread
                    : v == 2 ? g_vtoc
                             : g_vcfg;
    if (tgt) lv_obj_clear_flag(tgt, LV_OBJ_FLAG_HIDDEN);
    if (v == 2) toc_refresh();
    if (v == 3) cfg_refresh();
}

/* 书架视图组装 */
static void build_shelf(lv_obj_t *parent)
{
    lv_obj_t *tt = ui_label(parent, LV_SYMBOL_LIST " 小说阅读器",
                            &lv_font_cn_16, lv_color_hex(RD_TXT));
    lv_obj_align(tt, LV_ALIGN_TOP_MID, -60, 14);
    /* 设置入口（右上角齿轮） */
    lv_obj_t *gear = lv_obj_create(parent);
    lv_obj_set_size(gear, 64, 40);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -12, 10);
    ui_style_card(gear);
    lv_obj_set_style_radius(gear, 16, 0);
    lv_obj_clear_flag(gear, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gear, gear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = ui_label(gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_16,
                            lv_color_hex(RD_DIM));
    lv_obj_center(gl);

    g_hint = ui_label(parent, "", &lv_font_cn_14, lv_color_hex(RD_DIM));
    lv_obj_center(g_hint);
    lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);

    g_rows = lv_obj_create(parent);
    lv_obj_set_size(g_rows, 440, 402);
    lv_obj_align(g_rows, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_opa(g_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_rows, 0, 0);
    lv_obj_set_style_pad_all(g_rows, 0, 0);
    lv_obj_set_style_pad_row(g_rows, 6, 0);
    lv_obj_set_flex_flow(g_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_rows, LV_SCROLLBAR_MODE_OFF);
}

static void gear_cb(lv_event_t *e)
{
    (void)e;
    show_view(3);
}

/* ---------------- tick / key3 / build / on_exit ---------------- */

static void key3_next(void) { page_next(+1); }

/* 四向快甩（ui_gesture_flick_add）：阅读页翻页；书架/目录/设置保持
 * 上滑返回。上滑/左滑=下一页，下滑/右滑=上一页。 */
static void reader_flick_cb(void *user, int dir)
{
    (void)user;
    if (g_view == 1) {
        if (g_menu_open) return; /* 菜单开着：点按关闭，不翻页 */
        if (dir == UI_GESTURE_UP || dir == UI_GESTURE_LEFT)
            page_next(+1);
        else if (dir == UI_GESTURE_DOWN || dir == UI_GESTURE_RIGHT)
            page_next(-1);
        return;
    }
    if (dir == UI_GESTURE_UP) {
        if (g_view == 2 || g_view == 3)
            show_view(1);      /* 目录/设置 → 回阅读页 */
        else
            app_manager_back(); /* 书架 → 退出到桌面 */
    }
}

static void reader_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_root) return;
    /* 自动翻页（设置 auto_sec>0 且阅读视图） */
    if (g_view == 1 && !g_menu_open && !g_busy && g_cfg.auto_sec > 0) {
        if (--g_auto_left <= 0) page_next(+1);
    }
    /* 进度 debounce 落盘（5s 脏标记） */
    static int save_wait;
    if (g_save_dirty && ++save_wait >= 5) {
        save_wait = 0;
        g_save_dirty = 0;
        if (ui_worker_start(save_worker, NULL, 12 * 1024) != 0)
            g_save_dirty = 1;
    }
}

/* ---------------- 视图容器工厂与主题应用 ---------------- */

static lv_obj_t *make_view(lv_obj_t *scr)
{
    lv_obj_t *v = lv_obj_create(scr);
    lv_obj_set_size(v, 480, 480);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    return v;
}

/* 阅读视图主题底色（主题/字号变化时调） */
static void apply_theme_all(void)
{
    if (!g_vread) return;
    lv_obj_set_style_bg_color(g_vread,
                              lv_color_hex(theme_bg[g_cfg.theme]), 0);
    lv_obj_set_style_bg_opa(g_vread, LV_OPA_COVER, 0);
    if (g_chbar)
        lv_obj_set_style_text_color(g_chbar,
                                    lv_color_hex(theme_dim[g_cfg.theme]),
                                    0);
    if (g_foot)
        lv_obj_set_style_text_color(g_foot,
                                    lv_color_hex(theme_dim[g_cfg.theme]),
                                    0);
    apply_page_text();
}

/* ---------------- 目录视图容器 ---------------- */


static void build_toc(lv_obj_t *parent)
{
    lv_obj_t *back = lv_obj_create(parent);
    lv_obj_set_size(back, 108, 36);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_read_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, LV_SYMBOL_LEFT " 返回", &lv_font_cn_14,
                            lv_color_hex(RD_DIM));
    lv_obj_center(bl);

    lv_obj_t *tt = ui_label(parent, "目录", &lv_font_cn_16,
                            lv_color_hex(RD_TXT));
    lv_obj_align(tt, LV_ALIGN_TOP_MID, 0, 12);

    g_toc_rows = lv_obj_create(parent);
    lv_obj_set_size(g_toc_rows, 440, 402);
    lv_obj_align(g_toc_rows, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(g_toc_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_toc_rows, 0, 0);
    lv_obj_set_style_pad_all(g_toc_rows, 0, 0);
    lv_obj_set_style_pad_row(g_toc_rows, 6, 0);
    lv_obj_set_flex_flow(g_toc_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_toc_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_toc_rows, LV_SCROLLBAR_MODE_OFF);
}

/* ---------------- build / on_exit ---------------- */

static void shelf_start(void)
{
    if (g_shelf_busy) return;
    g_shelf_busy = 1;
    /* 100+ 本 + 三层子目录的扫描不是瞬时的：先给提示 */
    if (g_hint) {
        lv_obj_clear_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_hint, "正在整理书架…");
    }
    if (ui_worker_start(shelf_worker, (void *)(intptr_t)g_gen,
                        16 * 1024) != 0) {
        g_shelf_busy = 0;
        if (g_hint) {
            lv_obj_clear_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_hint,
                              "内存紧张，书架扫描未启动\n退出重进再试");
        }
    }
}

/* worker：读配置 + 扫书架（build 后异步执行） */
static void *init_worker(void *arg)
{
    int gen = (int)(intptr_t)arg;
    reader_config_load(&g_cfg);
    if (gen == g_gen) lv_async_call(init_async, (void *)(intptr_t)gen);
    return NULL;
}

static void init_async(void *arg)
{
    int gen = (int)(intptr_t)arg;
    g_open_busy = 0;
    if (!g_root || gen != g_gen) return;
    apply_theme_all();
    cfg_refresh();
    shelf_start();
}

void ui_reader_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    /* 阅读器要四向快甩翻页：用 flick 监听替代全局"上滑返回"
     * （否则上滑翻页会直接退出 App，用户 2026-11-18 实测反馈） */
    ui_gesture_flick_add(scr, reader_flick_cb, NULL);
    g_root = scr;
    g_gen++;
    g_view = 0;
    g_busy = 0;
    g_shelf_busy = 0;
    g_open_busy = 0;
    g_menu_open = 0;
    g_save_dirty = 0;
    g_cur_ch = g_cur_pg = 0;
    reader_config_t def = { 20, 1, 0, 0 };
    g_cfg = def;

    g_page = malloc(sizeof(reader_page_t));
    if (g_page) memset(g_page, 0, sizeof(*g_page));

    g_vshelf = make_view(scr);
    build_shelf(g_vshelf);
    g_vread = make_view(scr);
    build_read(g_vread);
    build_menu(g_vread);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);
    g_vtoc = make_view(scr);
    build_toc(g_vtoc);
    g_vcfg = make_view(scr);
    build_cfg(g_vcfg);

    show_view(0);
    ui_key_set_key3_handler(key3_next);
    if (!g_rtimer)
        g_rtimer = app_manager_timer_create(reader_tick_cb, 1000, NULL);
    if (ui_worker_start(init_worker, (void *)(intptr_t)g_gen, 16 * 1024) != 0)
        shelf_start();
    demo_hooks();
}

void ui_reader_on_exit(void)
{
    g_root = NULL;
    g_vshelf = g_vread = g_vtoc = g_vcfg = NULL;
    g_rows = g_hint = NULL;
    g_chbar = g_foot = g_toc_rows = NULL;
    g_cfg_rows = NULL;
    g_menu = NULL;
    for (int i = 0; i < RD_MAX_LINES; i++) g_txt[i] = NULL;
    /* 退出即清 RAM（2026-11-18 用户要求）：gen++ 作废在途翻页，zombie
     * 移交 teardown worker（进度落盘 + 延迟释放） */
    reader_scan_cancel(); /* 在途书架扫描（100+ 本）快速中止，防 UAF */
    if (!g_shelf_busy) reader_progress_cache_clear();
    g_gen++;
    g_busy = 0;
    if (g_book || g_page || g_shelf) {
        rd_zombie_t *z = malloc(sizeof(*z));
        if (z) {
            z->b = g_book;
            z->pg = g_page;
            z->sh = g_shelf;
            z->dirty = g_save_dirty;
        }
        g_book = NULL;
        g_page = NULL;
        g_shelf = NULL;
        g_shelf_n = 0;
        g_save_dirty = 0;
        if (ui_worker_start(reader_teardown_worker, z, 12 * 1024) != 0) {
            rd_zombie_free(z); /* 退化为同步释放 */
        }
    }
    ui_key_set_key3_handler(NULL);
    g_rtimer = NULL; /* app_manager 登记 timer，切走自动删 */
}

/* ---------------- 模拟器 demo 钩子 ---------------- */

static void demo_hooks(void)
{
    if (getenv("GADGET_DEMO_READER")) shelf_start();
    const char *op = getenv("GADGET_DEMO_READER_OPEN");
    if (op && op[0] >= '0' && op[0] <= '9') {
        g_demo_idx = atoi(op);
        g_demo_open = 1;
        shelf_start();
    }
}
