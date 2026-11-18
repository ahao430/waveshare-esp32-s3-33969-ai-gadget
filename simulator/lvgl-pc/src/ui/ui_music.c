/* ui_music.c — 本地音乐播放器（Hi-Fi 数字唱片机，蓝紫霓虹）。
 * 深色 AMOLED + 蓝紫主题（不跟全局主题切换）；唱片区旋转 + 进度弧 +
 * 频谱 + 五键控制；曲库二级视图（ui_files.c 骨架）；后台常驻播放
 * （番茄钟模式：常驻 timer + g_root 守卫 + 药丸）。结构见 DECISIONS。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include <lvgl.h>
#include "app_manager.h"
#include "data_source.h"
#include "ui_gesture.h"
#include "ui.h"
#include "ui_statusbar.h"
#include "ui_theme.h"
#include "music_player.h"

#include "ui_worker.h"

/* ---------------- 蓝紫局部主题 ---------------- */

#define MM_MAIN  0x7C6CFF   /* 主紫 */
#define MM_SUB   0x38BDF8   /* 辅青 */
#define MM_TXT   0xEAF0FF
#define MM_DIM   0xC3CBE8
#define MM_DIM2  0x8A93AD
#define MM_BG    0x0D0D14

#define VINY_D 160          /* 唱片直径 */
#define VINY_Y 62           /* 唱片容器顶 y */
#define BARS_N MUSIC_BARS

/* ---------------- 引用 ---------------- */

static lv_obj_t *g_root;      /* App 屏幕（on_exit 置 NULL） */
static lv_obj_t *g_vp;        /* 播放页容器 */
static lv_obj_t *g_vlib;      /* 曲库容器 */
static lv_obj_t *g_vinyl;     /* 旋转唱片 */
static lv_obj_t *g_arc;       /* 进度弧载体（DRAW_POST 自绘） */
static int g_arc_pct;         /* 进度百分比（draw_cb 取色/长度） */
static lv_obj_t *g_title, *g_artist, *g_badge;
static lv_obj_t *g_badge_lbl;
static lv_obj_t *g_bars[BARS_N];
static lv_obj_t *g_t_time, *g_t_total;
static lv_obj_t *g_t_cur_name;
static lv_obj_t *g_btn_play;    /* 中央键（符号随状态切 PLAY/PAUSE） */
static lv_obj_t *g_btn_play_lbl;
static lv_obj_t *g_btn_shuf, *g_btn_loop; /* 左右小键（二态高亮） */
static lv_obj_t *g_btn_shuf_lbl, *g_btn_loop_lbl;
static lv_obj_t *g_lib_rows;    /* 曲库列表容器 */
static lv_obj_t *g_lib_hint;    /* 空态/错误提示 */
static lv_timer_t *g_timer;     /* 后台常驻（ADR-011，番茄钟模式） */
static int g_gen;               /* 曲库 worker 代际 */
static volatile int g_lib_busy;
static char s_open_pending[MUSIC_PATH_MAX]; /* 文件管理器联动（ADR-019） */
static int g_rot_deg;           /* 光点轨道角（0..359） */
static lv_obj_t *g_dot;         /* 旋转高光点（模拟唱片反光） */
static int g_view;              /* 0=播放 1=曲库（前向声明防重复） */

/* ---------------- 进度弧（DRAW_POST 渐变自绘，订阅页同款） ---------------- */

static void arc_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int r = (coords.x2 - coords.x1) / 2;
    int cx = (coords.x1 + coords.x2) / 2;
    int cy = (coords.y1 + coords.y2) / 2;
    int pct = g_arc_pct;
    if (pct <= 0) return;
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.width = 6;
    dsc.rounded = 1;
    dsc.opa = LV_OPA_COVER;
    dsc.center.x = cx;
    dsc.center.y = cy;
    dsc.radius = r - 12;  /* 贴唱片外缘（wrap 有 12px 余量） */
    const int seg = 24;
    float sweep = 360.0f * pct / 100.0f;
    for (int i = 0; i < seg; i++) {
        float f0 = (float)i / seg, f1 = (float)(i + 1) / seg;
        float mid = (f0 + f1) / 2;
        dsc.color = lv_color_mix(lv_color_hex(MM_SUB), lv_color_hex(MM_MAIN),
                                 (uint8_t)(255 * mid));
        float a0 = 270.0f + sweep * f0;
        float a1 = 270.0f + sweep * f1;
        if (i > 0) a0 -= 0.8f;
        if (i < seg - 1) a1 += 0.0f;
        dsc.start_angle = a0;
        dsc.end_angle = a1;
        lv_draw_arc(layer, &dsc);
    }
}

/* ---------------- 曲库（ui_files.c 骨架：worker+代际+行渲染） ---------------- */


static void show_view(int v);
static void lib_row_cb(lv_event_t *e);
static void show_view0_cb(lv_event_t *e);

static void lib_async(void *arg)
{
    int gen = (int)(intptr_t)arg;
    if (gen != g_gen) return;            /* 页面已退出/重扫：丢弃 */
    if (!g_lib_rows) {
        g_lib_busy = 0;                  /* 防 busy 卡死导致之后永不再扫 */
        return;
    }
    lv_obj_clean(g_lib_rows);
    g_lib_busy = 0;
#ifdef ESP_PLATFORM
    ESP_LOGI("music", "lib view: %d tracks", music_player_count());
#endif
    /* 文件管理器联动：曲库命中即切播（ADR-019） */
    if (s_open_pending[0]) {
        for (int i = 0; i < music_player_count(); i++) {
            const music_track_t *tk = music_player_track(i);
            if (tk && strstr(tk->path, s_open_pending)) {
                music_player_play(i);
                break;
            }
        }
        s_open_pending[0] = 0;
    }
    int n = music_player_count();
    if (n == 0) {
        if (g_lib_hint) {
            lv_obj_clear_flag(g_lib_hint, LV_OBJ_FLAG_HIDDEN);
            const char *note = music_player_scan_note();
            lv_label_set_text(g_lib_hint,
                              g_root ? (note[0] ? note
                                       : "TF 卡 music/ 目录没有可播的\nMP3/WAV 文件")
                                     : "");
        }
        return;
    }
    if (g_lib_hint) lv_obj_add_flag(g_lib_hint, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < n; i++) {
        const music_track_t *t = music_player_track(i);
        lv_obj_t *row = lv_obj_create(g_lib_rows);
        lv_obj_set_size(row, LV_PCT(100), 46);
        ui_style_card(row);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, lib_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *ic = ui_label(row, t->fmt[0] == 'M' ? LV_SYMBOL_AUDIO : LV_SYMBOL_DRIVE,
                                &lv_font_montserrat_16, lv_color_hex(MM_SUB));
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *nm = ui_label(row, t->title, &lv_font_cn_14,
                                lv_color_hex(MM_TXT));
        lv_obj_set_width(nm, 280);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 38, 0);
        char dur[16];
        snprintf(dur, sizeof(dur), "%d:%02d", t->duration_s / 60,
                 t->duration_s % 60);
        lv_obj_t *du = ui_label(row, dur, &lv_font_cn_12, lv_color_hex(MM_DIM2));
        lv_obj_align(du, LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

static void *lib_worker(void *arg)
{
    int gen = (int)(intptr_t)arg;
    music_player_scan();
    if (gen == g_gen) lv_async_call(lib_async, (void *)(intptr_t)gen);
    return NULL;
}

static void lib_start(void)
{
    if (g_lib_busy) return;
    g_lib_busy = 1;
    g_gen++;
    music_player_init();
    /* 扫描要读 20+ 个文件的 ID3 头，真机上不是瞬时的：先给"扫描中"
     * 提示，避免用户以为曲库读不出来 */
    if (g_lib_hint) {
        lv_obj_clear_flag(g_lib_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_lib_hint, "正在扫描 music/ …");
    }
    if (ui_worker_start(lib_worker, (void *)(intptr_t)g_gen, 16 * 1024) != 0) {
        g_lib_busy = 0;
        /* 内部 RAM 碎片化时 pthread 栈可能建不出来（坑 #13）：明确提示，
         * 不要留下一个永远空白的曲库（2026-11-18 用户反馈读目录失败） */
        if (g_lib_hint) {
            lv_obj_clear_flag(g_lib_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_lib_hint,
                              "内存紧张，曲库扫描未启动\n退出重进曲库再试");
        }
    }
}

/* 点曲库行：切歌并回播放页 */
static void lib_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    music_player_play(idx);
    show_view(0);
}

static void lib_open_cb(lv_event_t *e)
{
    (void)e;
    show_view(1);
}

/* ---------------- 常驻 timer（旋转/频谱/进度/药丸） ---------------- */

static void sync_statusbar(void)
{
    const music_track_t *t = music_player_track(music_player_cur_idx());
    if (music_player_state() == MUSIC_STOPPED || !t) {
        ui_statusbar_set_app_status(NULL, 0);
        return;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), LV_SYMBOL_PLAY " %.60s", t->title);
    ui_statusbar_set_app_status(buf, MM_MAIN);
}

static void music_tick_cb(lv_timer_t *t)
{
    (void)t;
    /* 前台：旋转 + 频谱 + 进度 + 信息行 */
    if (g_root && g_view == 0) {
        if (music_player_state() == MUSIC_PLAYING) {
            /* 唱片本体静态（SW transform 整图旋转在软渲染下 CPU 爆表，
             * 2026-11-18 sample 实测 lv_draw_sw_transform 占满主线程），
             * 用 24px 高光点绕圆周转模拟反光——零 transform 成本 */
            g_rot_deg = (g_rot_deg + 6) % 360;
            if (g_dot) {
                int rad = VINY_D / 2 - 14;
                double a = g_rot_deg * 3.14159265 / 180.0;
                lv_obj_align(g_dot, LV_ALIGN_TOP_MID,
                             (int)(rad * cos(a)), (int)(rad * sin(a)));
                lv_obj_set_style_bg_opa(g_dot, LV_OPA_60, 0);
            }
        }
        uint8_t bars[MUSIC_BARS];
        music_player_get_spectrum(bars);
        for (int i = 0; i < BARS_N; i++) {
            if (!g_bars[i]) continue;
            int h = 4 + bars[i] * 52 / 255;
            lv_obj_set_size(g_bars[i], 6, h);
        }
        int cur_s, tot_s;
        music_player_get_progress(&cur_s, &tot_s);
        if (tot_s > 0) g_arc_pct = cur_s * 100 / tot_s;
        if (g_t_time && g_t_total) {
            char b[16];
            snprintf(b, sizeof(b), "%02d:%02d", cur_s / 60, cur_s % 60);
            lv_label_set_text(g_t_time, b);
            snprintf(b, sizeof(b), "%02d:%02d", tot_s / 60, tot_s % 60);
            lv_obj_set_style_text_color(g_t_total, lv_color_hex(MM_DIM2), 0);
            lv_label_set_text(g_t_total, b);
        }
        /* 信息行随曲目更新 */
        const music_track_t *tk = music_player_track(music_player_cur_idx());
        if (tk) {
            if (g_title) lv_label_set_text(g_title, tk->title);
            if (g_artist) lv_label_set_text(g_artist, tk->artist);
            if (g_badge_lbl) lv_label_set_text(g_badge_lbl, tk->fmt);
        }
        /* 中央键符号随状态切换 */
        if (g_btn_play_lbl) {
            lv_label_set_text(g_btn_play_lbl,
                music_player_state() == MUSIC_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }
    /* 前后台都做：曲终自动切歌后信息行/药丸刷新 */
    sync_statusbar();
}

/* ---------------- 视图切换与控制回调 ---------------- */

static void show_view(int v)
{
    g_view = v;
    if (g_root) {
        lv_obj_add_flag(g_vp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_vlib, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *tgt = v == 0 ? g_vp : g_vlib;
        lv_obj_clear_flag(tgt, LV_OBJ_FLAG_HIDDEN);
        if (v == 1) lib_start(); /* 每次进曲库都重扫+重渲染（worker 异步） */
    }
}

static void prev_cb(lv_event_t *e) { (void)e; music_player_next(-1); }
static void next_cb(lv_event_t *e) { (void)e; music_player_next(+1); }
static void shuf_cb(lv_event_t *a)
{
    (void)a;
    music_player_set_shuffle(!music_player_get_shuffle());
}
static void loop_cb(lv_event_t *e)
{
    (void)e;
    music_player_cycle_loop();
}
static void play_cb(lv_event_t *e)
{
    (void)e;
    music_player_toggle();
}

/* ---------------- 播放页构建 ---------------- */

static lv_obj_t *round_btn(lv_obj_t *parent, int size, int x, int y,
                           const char *sym, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1B1B2A), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(b, sym, &lv_font_montserrat_20, lv_color_hex(color));
    lv_obj_center(l);
    return b;
}

/* ---------------- 播放页构建 ---------------- */

static void build_player(lv_obj_t *parent)
{
    /* 顶行：标题 + 曲库入口 */
    lv_obj_t *top = lv_obj_create(parent);
    lv_obj_set_size(top, 448, 44);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ic = ui_label(top, LV_SYMBOL_AUDIO, &lv_font_montserrat_20, lv_color_hex(MM_MAIN));
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *tt = ui_label(top, "本地音乐", &lv_font_cn_16, lv_color_hex(MM_TXT));
    lv_obj_align(tt, LV_ALIGN_LEFT_MID, 36, 0);
    lv_obj_t *libbtn = lv_obj_create(top);
    lv_obj_set_size(libbtn, 96, 32);
    lv_obj_align(libbtn, LV_ALIGN_RIGHT_MID, 0, 0);
    ui_style_card(libbtn);
    lv_obj_set_style_radius(libbtn, 16, 0);
    lv_obj_add_flag(libbtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(libbtn, lib_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb = ui_label(libbtn, LV_SYMBOL_LIST " 曲库", &lv_font_cn_14, lv_color_hex(MM_DIM));
    lv_obj_center(lb);

    /* 唱片区：旋转纹理 + 外圈进度弧 + 中心封面圆 */
    lv_obj_t *vinylwrap = lv_obj_create(parent);
    lv_obj_set_size(vinylwrap, 192, 192);
    lv_obj_align(vinylwrap, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_opa(vinylwrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vinylwrap, 0, 0);
    lv_obj_clear_flag(vinylwrap, LV_OBJ_FLAG_SCROLLABLE);
    g_arc = vinylwrap; /* 进度弧画在 wrap 上（DRAW_POST） */
    lv_obj_add_event_cb(vinylwrap, arc_draw_cb, LV_EVENT_DRAW_POST, NULL);

    /* 黑胶唱片 = 纯 LVGL 对象绘制（静态 RGB565 dsc 在模拟器 9.2 渲染黑屏
     * 实证弃用；且省 50KB rodata）。不均匀纹理弧保证"旋转可见性"（高光点
     * 绕行时穿过不同亮度的纹理弧）。 */
    lv_obj_t *disc = lv_obj_create(vinylwrap);
    lv_obj_set_size(disc, VINY_D, VINY_D);
    lv_obj_center(disc);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(0x15151D), 0);
    lv_obj_set_style_bg_grad_color(disc, lv_color_hex(0x23232E), 0);
    lv_obj_set_style_bg_grad_dir(disc, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(disc, 0, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    /* 同心纹理槽：宽 1~2px 的细弧，亮度/缺口角度各不相同 */
    static const struct { int r; int a0; int a1; int c; int w; } rings[] = {
        { 62, 0, 360, 0x2A2A33, 1 }, { 58, 0, 360, 0x24242C, 1 },
        { 54, 210, 40, 0x30303B, 1 }, { 50, 0, 360, 0x212129, 1 },
        { 46, 40, 150, 0x35353F, 1 }, { 42, 0, 360, 0x25252D, 1 },
        { 38, 130, 250, 0x2E2E38, 1 }, { 34, 0, 360, 0x22222A, 1 },
        { 66, 300, 60, 0x3A3A45, 2 }, { 70, 90, 200, 0x40404B, 2 },
    };
    for (size_t i = 0; i < sizeof(rings) / sizeof(rings[0]); i++) {
        lv_obj_t *ar = lv_arc_create(disc);
        lv_obj_set_size(ar, rings[i].r * 2, rings[i].r * 2);
        lv_obj_center(ar);
        lv_arc_set_rotation(ar, 270);
        lv_arc_set_bg_angles(ar, 0, 360);
        lv_arc_set_value(ar, 100);
        lv_obj_remove_style(ar, NULL, LV_PART_INDICATOR);
        lv_obj_remove_style(ar, NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_color(ar, lv_color_hex(rings[i].c), LV_PART_MAIN);
        lv_obj_set_style_arc_width(ar, rings[i].w, LV_PART_MAIN);
        lv_arc_set_start_angle(ar, rings[i].a0 - 90);
        lv_arc_set_end_angle(ar, rings[i].a1 - 90);
        lv_obj_clear_flag(ar, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 中心封面圆（渐变底 + 音符） */
    lv_obj_t *cover = lv_obj_create(vinylwrap);
    lv_obj_set_size(cover, 76, 76);
    lv_obj_center(cover);
    lv_obj_set_style_radius(cover, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x141428), 0);
    lv_obj_set_style_bg_grad_color(cover, lv_color_hex(0x2A2A55), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(cover, 2, 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(MM_MAIN), 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *note = ui_label(cover, LV_SYMBOL_AUDIO, &lv_font_montserrat_28, lv_color_hex(MM_SUB));
    lv_obj_center(note);

    /* 旋转高光点（24px，透明圆） */
    g_dot = lv_obj_create(vinylwrap);
    lv_obj_set_size(g_dot, 24, 24);
    lv_obj_set_style_radius(g_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_dot, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_dot, 0, 0);
    lv_obj_clear_flag(g_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_dot, LV_OBJ_FLAG_CLICKABLE);

    /* 信息行：歌名 / 歌手 / 格式徽章 */
    g_title = ui_label(parent, "--", &lv_font_cn_16, lv_color_hex(MM_TXT));
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 264);
    g_artist = ui_label(parent, "", &lv_font_cn_12, lv_color_hex(MM_DIM));
    lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 288);
    g_badge = lv_obj_create(parent);
    lv_obj_set_size(g_badge, 64, 22);
    lv_obj_align(g_badge, LV_ALIGN_TOP_MID, 0, 310);
    lv_obj_set_style_bg_color(g_badge, lv_color_hex(0x1B1B2A), 0);
    lv_obj_set_style_radius(g_badge, 11, 0);
    lv_obj_set_style_border_width(g_badge, 0, 0);
    lv_obj_clear_flag(g_badge, LV_OBJ_FLAG_SCROLLABLE);
    g_badge_lbl = ui_label(g_badge, "MP3", &lv_font_cn_12, lv_color_hex(MM_SUB));
    lv_obj_center(g_badge_lbl);

    /* 频谱区：16 条底对齐竖条 */
    for (int i = 0; i < BARS_N; i++) {
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_set_size(b, 6, 8);
        lv_obj_align(b, LV_ALIGN_TOP_MID, -120 + i * 16, 338);
        lv_obj_set_style_bg_color(b, lv_color_hex(MM_SUB), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(MM_MAIN), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        g_bars[i] = b;
    }

    /* 进度时间 + 五键 */
    g_t_time = ui_label(parent, "00:00", &lv_font_montserrat_20, lv_color_hex(MM_TXT));
    lv_obj_align(g_t_time, LV_ALIGN_TOP_MID, -176, 352);
    g_t_total = ui_label(parent, "00:00", &lv_font_montserrat_20, lv_color_hex(MM_DIM2));
    lv_obj_align(g_t_total, LV_ALIGN_TOP_MID, 176, 352);
    g_btn_shuf = round_btn(parent, 52, -120, 398, LV_SYMBOL_SHUFFLE, MM_DIM2, shuf_cb);
    g_btn_shuf_lbl = lv_obj_get_child(g_btn_shuf, 0);
    round_btn(parent, 52, -60, 398, LV_SYMBOL_PREV, MM_TXT, prev_cb);
    g_btn_play = round_btn(parent, 76, 0, 386, LV_SYMBOL_PLAY, MM_TXT, play_cb);
    g_btn_play_lbl = lv_obj_get_child(g_btn_play, 0);
    lv_obj_set_style_bg_color(g_btn_play, lv_color_hex(0x232345), 0);
    round_btn(parent, 52, 60, 398, LV_SYMBOL_NEXT, MM_TXT, next_cb);
    g_btn_loop = round_btn(parent, 52, 120, 398, LV_SYMBOL_LOOP, MM_DIM2, loop_cb);
    g_btn_loop_lbl = lv_obj_get_child(g_btn_loop, 0);
}

/* ---------------- 曲库视图 ---------------- */

static void build_lib(lv_obj_t *parent)
{
    /* ui_files.c 同构：标题/按钮直接挂容器（不套 top 卡），列表容器
     * TRANSP + flex column；行 LV_PCT(100)+clear SCROLLABLE */
    lv_obj_t *back = lv_obj_create(parent);
    lv_obj_set_size(back, 108, 36);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, show_view0_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, LV_SYMBOL_LEFT " 返回", &lv_font_cn_14,
                            lv_color_hex(MM_DIM));
    lv_obj_center(bl);

    lv_obj_t *tt = ui_label(parent, "曲库", &lv_font_cn_16, lv_color_hex(MM_TXT));
    lv_obj_align(tt, LV_ALIGN_TOP_MID, 26, 12);

    g_lib_hint = ui_label(parent, "", &lv_font_cn_14, lv_color_hex(MM_DIM));
    lv_obj_center(g_lib_hint);

    g_lib_rows = lv_obj_create(parent);
    lv_obj_set_size(g_lib_rows, 440, 400);
    lv_obj_align(g_lib_rows, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_opa(g_lib_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_lib_rows, 0, 0);
    lv_obj_set_style_pad_all(g_lib_rows, 0, 0);
    lv_obj_set_style_pad_row(g_lib_rows, 6, 0);
    lv_obj_set_flex_flow(g_lib_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_lib_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_lib_rows, LV_SCROLLBAR_MODE_OFF);
}

static void show_view0_cb(lv_event_t *e)
{
    (void)e;
    show_view(0);
}

/* ---------------- App 入口（build / on_exit / timer） ---------------- */

static void demo_hooks(void)
{
    if (getenv("GADGET_DEMO_MUSIC_LIB")) {
        music_player_scan();
        show_view(1);
    }
    const char *p = getenv("GADGET_DEMO_MUSIC_PLAY");
    if (p && p[0] == '1') {
        music_player_scan();
        if (music_player_count() == 0) return;
        music_player_play(0);
    }
}

void ui_music_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    music_player_init();
    ui_gesture_back_add(scr);
    g_root = scr;
    g_arc_pct = 0;
    g_rot_deg = 0;
    memset(g_bars, 0, sizeof(g_bars));

    /* 播放页 + 曲库两个视图容器（叠放，show_view 切换） */
    g_vp = lv_obj_create(scr);
    lv_obj_set_size(g_vp, 480, 480);
    lv_obj_set_style_bg_opa(g_vp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_vp, 0, 0);
    lv_obj_clear_flag(g_vp, LV_OBJ_FLAG_SCROLLABLE);
    g_vlib = lv_obj_create(scr);
    lv_obj_set_size(g_vlib, 480, 480);
    lv_obj_set_style_bg_opa(g_vlib, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_vlib, 0, 0);
    lv_obj_clear_flag(g_vlib, LV_OBJ_FLAG_SCROLLABLE);
    build_lib(g_vlib);
    build_player(g_vp);
    show_view(0);
    demo_hooks();
    /* 文件管理器联动（ADR-019）：带曲目路径打开 → 立即扫描，命中后在
     * lib_async 里切播（否则 pending 要等用户手动进曲库才消费） */
    if (s_open_pending[0]) lib_start();
    if (!g_timer) g_timer = lv_timer_create(music_tick_cb, 200, NULL);
    /* 后台常驻 timer 不登记 app_manager（ADR-011 番茄钟模式） */
}

void ui_music_on_exit(void)
{
    /* 在途曲库扫描：代际 +1 丢弃回调，并复位 busy——否则 worker 回来时
     * g_lib_rows 已 NULL，busy 永远卡 1，下次进曲库 lib_start 直接返回：
     * 曲库永远空白（2026-11-18 真机反馈"读取 music 目录失败"根因） */
    g_gen++;
    g_lib_busy = 0;
    g_root = NULL;
    g_vp = g_vlib = NULL;
    g_vinyl = g_arc = g_dot = NULL;
    for (int i = 0; i < BARS_N; i++) g_bars[i] = NULL;
    g_t_time = g_t_total = NULL;
    g_lib_rows = g_lib_hint = NULL;
    g_btn_play = g_btn_play_lbl = NULL;
    g_btn_shuf = g_btn_loop = NULL;
    g_btn_shuf_lbl = g_btn_loop_lbl = NULL;
    /* timer 常驻不删（后台播放继续刷药丸，番茄钟模式） */
}

/* 文件管理器联动入口（ADR-019） */
void ui_music_open_path(const char *rel)
{
    snprintf(s_open_pending, sizeof(s_open_pending), "%s", rel ? rel : "");
}

/* 无触摸自检（GADGET_SELFTEST_MUSIC）：直接切曲库并触发扫描 */
void ui_music_debug_lib(void)
{
    if (g_root) show_view(1);
}
