/* ui_pomodoro — 番茄钟 App v3（深色 AMOLED 产品化改版，2026-11-17）。
 *
 * 设计稿 assets/ui/番茄时钟.png：番茄红橙渐变圆环 + 立体番茄吉祥物
 * （assets/ui/番茄专注休息表情包.png 四态，构建期 RGB565 预解码）+
 * 大号倒计时 + 模式分段控件 + 今日统计 + 底部重置/暂停双钮。
 *
 * 状态机/统计/后台计时沿用 v2（ADR-011：lv_timer 常驻，退 App 只断
 * UI 引用）：专注 →（每 4 轮）长休息 → 专注；手动切模式走分段控件。
 * 视觉语义：运行=满亮渐变环；暂停=环压暗；阶段完成=完成态吉祥物 +
 * 环呼吸（等用户开启下一段）。
 *
 * 统计与配置：`<TF>/data/pomodoro.json`（今日/累计次数+分钟、三段
 * 时长配置）。IO 一律 worker + lv_async_call（坑 #11）。
 */
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "app_manager.h"
#include "data_source.h"
#include "fonts/ui_font_cn.h"
#include "icons/pomo_mascot.h"
#include "ui.h"
#include "ui_gesture.h"
#include "ui_sound.h"
#include "ui_statusbar.h"
#include "ui_theme.h"

#define TICK_MS 200
#define CYCLE_LEN 4

enum { PH_FOCUS = 0, PH_SHORT = 1, PH_LONG = 2 };

/* 深色 AMOLED 配色（设计稿取值） */
#define POMO_BG      0x0D0D14 /* AMOLED 黑 */
#define POMO_CARD    0x1E1F2E /* 胶囊容器 */
#define POMO_CARD2   0x23243A /* 次级容器/重置钮 */
#define POMO_DIM     0x8B8CA7 /* 环内副题 */
#define POMO_TEXT2   0x9CA0B5 /* 统计正文 */
#define POMO_ORANGE  0xF97316 /* 强调橙 */
#define POMO_RED     0xF43F5E /* 番茄红 */
#define POMO_PEACH   0xFDBA74 /* 亮橙（渐变端） */
#define POMO_CYAN    0x22D3EE /* 短休息 */
#define POMO_BLUE    0x3B82F6
#define POMO_PURPLE  0xA855F7 /* 长休息 */

static int g_phase = PH_FOCUS;
static int g_remaining = 25 * 60; /* 剩余秒（页面退出/重建不丢） */
static int g_streak;              /* 本轮已完成专注数（0..CYCLE_LEN-1） */
static int g_total_override;      /* 回归钩子：短时长演示覆盖阶段总长 */
static bool g_running;
static uint32_t g_deadline;       /* 运行中：剩余归零的 lv_tick */
static int g_shown_sec = -1;
static bool g_just_done;          /* 阶段刚完成：完成态吉祥物+环呼吸 */
static int g_ring_opa = 255;      /* 环亮度（暂停压暗/完成呼吸） */

/* 时长配置（分钟，持久化到 pomodoro.json；缺省 25/5/15） */
static int g_cfg_focus = 25, g_cfg_short = 5, g_cfg_long = 15;

static lv_timer_t *g_timer; /* 后台常驻（不随页面销毁，ADR-011） */
static lv_obj_t *g_root, *g_sub_lbl, *g_stats_lbl, *g_arc;
static lv_obj_t *g_mascot, *g_gear;
static lv_obj_t *g_seg[3]; /* 模式分段控件（专注/休息/长休） */
static lv_obj_t *g_btn_play, *g_lbl_play;
static lv_obj_t *g_digits[4];
static bool g_demo_used;

/* ---------------- 统计 + 配置（TF: data/pomodoro.json） ---------------- */
typedef struct {
    int today;
    int total;
    int today_min; /* 今日累计专注分钟 */
    int total_min; /* 全部累计专注分钟 */
    int focus_min, short_min, long_min; /* 时长配置（0=用默认） */
    char date[11]; /* YYYY-MM-DD */
} pomo_stats_t;

static pomo_stats_t g_stats;
static bool g_stats_loaded, g_stats_loading, g_saving, g_save_pending;
static pthread_mutex_t g_stats_mu = PTHREAD_MUTEX_INITIALIZER;

static int phase_total(void)
{
    if (g_total_override > 0) return g_total_override;
    if (g_phase == PH_SHORT) return g_cfg_short * 60;
    if (g_phase == PH_LONG) return g_cfg_long * 60;
    return g_cfg_focus * 60;
}

/* 各阶段视觉主色对（环渐变 / 分段选中态 / 状态栏药丸） */
static void phase_colors(uint32_t *c0, uint32_t *c1)
{
    if (g_phase == PH_SHORT) {
        if (c0) *c0 = POMO_CYAN;
        if (c1) *c1 = POMO_BLUE; /* 短休息：青 → 蓝 */
    } else if (g_phase == PH_LONG) {
        if (c0) *c0 = POMO_PURPLE;
        if (c1) *c1 = POMO_BLUE; /* 长休息：紫 → 蓝 */
    } else {
        if (c0) *c0 = POMO_RED;
        if (c1) *c1 = POMO_PEACH; /* 专注：番茄红 → 亮橙 */
    }
}

static uint32_t phase_accent(void)
{
    uint32_t c0 = 0, c1 = 0;
    phase_colors(&c0, &c1);
    return c0;
}

static const char *phase_title(void)
{
    if (g_phase == PH_SHORT) return "短休息";
    if (g_phase == PH_LONG) return "长休息";
    return "专注时间";
}

static const lv_image_dsc_t *phase_mascot(void)
{
    if (g_just_done) return &pomo_mascot_done; /* 完成：欢呼庆祝 */
    if (g_phase == PH_SHORT) return &pomo_mascot_rest;
    if (g_phase == PH_LONG) return &pomo_mascot_long;
    return &pomo_mascot_focus;
}

static void today_str(char out[11])
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d", tmv.tm_year + 1900,
                     tmv.tm_mon + 1, tmv.tm_mday);
    if (n < 0) n = 0;
    if (n > 10) n = 10;
    memcpy(out, tmp, (size_t)n);
    out[n] = '\0';
}

static bool stats_path(char *out, size_t cap)
{
    return ds_sd_resolve(out, (int)cap, "data/pomodoro.json") == 0;
}

static void stats_read(pomo_stats_t *out)
{
    pthread_mutex_lock(&g_stats_mu);
    *out = g_stats;
    pthread_mutex_unlock(&g_stats_mu);
}

static bool start_thread(void *(*fn)(void *), void *arg, size_t stack)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    int rc = pthread_create(&tid, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
    pthread_detach(tid);
    return true;
}

static void stats_request_save(void);
static void update_ui(bool phase_changed);

static void stats_saved_async(void *arg)
{
    (void)arg;
    g_saving = false;
    if (g_save_pending) {
        g_save_pending = false;
        stats_request_save();
    }
}

static void *stats_save_worker(void *arg)
{
    pomo_stats_t *s = (pomo_stats_t *)arg;
    char path[192];
    if (stats_path(path, sizeof(path))) {
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "date", s->date);
            cJSON_AddNumberToObject(root, "today", s->today);
            cJSON_AddNumberToObject(root, "total", s->total);
            cJSON_AddNumberToObject(root, "today_min", s->today_min);
            cJSON_AddNumberToObject(root, "total_min", s->total_min);
            cJSON_AddNumberToObject(root, "focus_min", s->focus_min);
            cJSON_AddNumberToObject(root, "short_min", s->short_min);
            cJSON_AddNumberToObject(root, "long_min", s->long_min);
            char *txt = cJSON_PrintUnformatted(root);
            if (txt) {
                char tmp[200];
                snprintf(tmp, sizeof(tmp), "%s.tmp", path);
                FILE *f = fopen(tmp, "wb");
                if (f) {
                    size_t n = strlen(txt);
                    bool ok = fwrite(txt, 1, n, f) == n;
                    fclose(f);
                    if (ok) rename(tmp, path); /* 原子替换，防掉电写半截 */
                }
                cJSON_free(txt);
            }
            cJSON_Delete(root);
        }
    }
    free(s);
    lv_async_call(stats_saved_async, NULL);
    return NULL;
}

static void stats_request_save(void)
{
    if (!g_stats_loaded || g_saving) {
        g_save_pending = true; /* 等 load 完成/上一次写完再落盘 */
        return;
    }
    pomo_stats_t *s = malloc(sizeof(*s));
    if (!s) return;
    pthread_mutex_lock(&g_stats_mu);
    *s = g_stats;
    pthread_mutex_unlock(&g_stats_mu);
    g_saving = true;
    if (!start_thread(stats_save_worker, s, 8 * 1024)) {
        free(s);
        g_saving = false;
    }
}

/* load 完成后把磁盘配置套进运行态（0=缺省值） */
static void cfg_apply_from_stats(const pomo_stats_t *s)
{
    if (s->focus_min >= 1 && s->focus_min <= 180) g_cfg_focus = s->focus_min;
    if (s->short_min >= 1 && s->short_min <= 60) g_cfg_short = s->short_min;
    if (s->long_min >= 1 && s->long_min <= 90) g_cfg_long = s->long_min;
}

static void cfg_write_to_stats(void)
{
    pthread_mutex_lock(&g_stats_mu);
    g_stats.focus_min = g_cfg_focus;
    g_stats.short_min = g_cfg_short;
    g_stats.long_min = g_cfg_long;
    pthread_mutex_unlock(&g_stats_mu);
}

static void stats_loaded_async(void *arg)
{
    (void)arg;
    char today[11];
    today_str(today);
    bool rollover = false;
    pthread_mutex_lock(&g_stats_mu);
    if (strcmp(g_stats.date, today) != 0) { /* 跨天：今日清零，累计保留 */
        snprintf(g_stats.date, sizeof(g_stats.date), "%s", today);
        g_stats.today = 0;
        g_stats.today_min = 0;
        rollover = true;
    }
    pthread_mutex_unlock(&g_stats_mu);
    g_stats_loading = false;
    g_stats_loaded = true;
    if (rollover) stats_request_save();
    else if (g_save_pending) {
        g_save_pending = false;
        stats_request_save();
    }
    update_ui(false);
}

static void *stats_load_worker(void *arg)
{
    (void)arg;
    pomo_stats_t st;
    memset(&st, 0, sizeof(st));
    char path[192];
    if (stats_path(path, sizeof(path))) {
        FILE *f = fopen(path, "rb");
        if (f) {
            char buf[768];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                const cJSON *v;
                if ((v = cJSON_GetObjectItem(root, "date")) && cJSON_IsString(v))
                    snprintf(st.date, sizeof(st.date), "%s", v->valuestring);
                if ((v = cJSON_GetObjectItem(root, "today")) && cJSON_IsNumber(v))
                    st.today = v->valueint;
                if ((v = cJSON_GetObjectItem(root, "total")) && cJSON_IsNumber(v))
                    st.total = v->valueint;
                if ((v = cJSON_GetObjectItem(root, "today_min")) && cJSON_IsNumber(v))
                    st.today_min = v->valueint;
                if ((v = cJSON_GetObjectItem(root, "total_min")) && cJSON_IsNumber(v))
                    st.total_min = v->valueint;
                if ((v = cJSON_GetObjectItem(root, "focus_min")) && cJSON_IsNumber(v))
                    st.focus_min = v->valueint;
                if ((v = cJSON_GetObjectItem(root, "short_min")) && cJSON_IsNumber(v))
                    st.short_min = v->valueint;
                if ((v = cJSON_GetObjectItem(root, "long_min")) && cJSON_IsNumber(v))
                    st.long_min = v->valueint;
                cJSON_Delete(root);
            }
        }
    }
    if (st.today < 0) st.today = 0;
    if (st.total < 0) st.total = 0;
    if (st.today_min < 0) st.today_min = 0;
    if (st.total_min < 0) st.total_min = 0;
    cfg_apply_from_stats(&st);

    /* 合并而不是覆盖：load 期间若已完成专注（演示短时长可能发生），
     * 内存里的新增不能被磁盘旧值抹掉；今日数只在同一天时取较大值。 */
    char today[11];
    today_str(today);
    pthread_mutex_lock(&g_stats_mu);
    if (st.date[0] && strcmp(st.date, today) == 0) {
        if (st.today > g_stats.today) g_stats.today = st.today;
        if (st.today_min > g_stats.today_min) g_stats.today_min = st.today_min;
    }
    if (st.total > g_stats.total) g_stats.total = st.total;
    if (st.total_min > g_stats.total_min) g_stats.total_min = st.total_min;
    if (g_stats.date[0] == '\0') {
        snprintf(g_stats.date, sizeof(g_stats.date), "%s", today);
    }
    pthread_mutex_unlock(&g_stats_mu);
    lv_async_call(stats_loaded_async, NULL);
    return NULL;
}

static void stats_add_focus(int minutes)
{
    char today[11];
    today_str(today);
    pthread_mutex_lock(&g_stats_mu);
    if (strcmp(g_stats.date, today) != 0) {
        snprintf(g_stats.date, sizeof(g_stats.date), "%s", today);
        g_stats.today = 0;
        g_stats.today_min = 0;
    }
    g_stats.today++;
    g_stats.total++;
    g_stats.today_min += minutes;
    g_stats.total_min += minutes;
    pthread_mutex_unlock(&g_stats_mu);
    stats_request_save();
}

/* 运行中按截止时间取剩余秒（向上取整：开始瞬间显示满时长） */
static int remaining_now(void)
{
    int32_t left = (int32_t)(g_deadline - lv_tick_get());
    return left > 0 ? (left + 999) / 1000 : 0;
}

static void set_digit(int i, char c)
{
    char s[2] = { c, '\0' };
    if (!g_digits[i]) return;
    if (strcmp(lv_label_get_text(g_digits[i]), s) != 0) {
        lv_label_set_text(g_digits[i], s);
    }
}

/* ---------------- 渐变圆环（DRAW_POST 分段自绘，同订阅页技法） ----------------
 * LVGL 9.2 arc 无原生渐变；36 段 + 0.8° 重叠消接缝。暂停压暗 / 完成
 * 呼吸通过把段色向背景色 mix 实现（g_ring_opa 0..255）。 */
static void pomo_arc_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    int total = phase_total();
    int pct = total > 0 ? g_remaining * 100 / total : 0;
    if (pct <= 0) return; /* 空弧不画（呼吸态也留空） */

    uint32_t h0, h1;
    phase_colors(&h0, &h1);
    lv_color_t bg = lv_color_hex(POMO_BG);
    lv_color_t c0 = lv_color_mix(lv_color_hex(h0), bg, (uint8_t)g_ring_opa);
    lv_color_t c1 = lv_color_mix(lv_color_hex(h1), bg, (uint8_t)g_ring_opa);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.width = 14;
    dsc.rounded = 1;
    dsc.opa = LV_OPA_COVER;
    dsc.center.x = (coords.x1 + coords.x2) / 2;
    dsc.center.y = (coords.y1 + coords.y2) / 2;
    dsc.radius = (coords.x2 - coords.x1) / 2;

    const int seg = 36;
    float sweep = 360.0f * (float)pct / 100.0f;
    for (int i = 0; i < seg; i++) {
        float f0 = (float)i / seg, f1 = (float)(i + 1) / seg;
        float mid = (f0 + f1) / 2;
        dsc.color = lv_color_mix(c1, c0, (uint8_t)(255 * mid));
        float a0 = 270.0f + sweep * f0;
        float a1 = 270.0f + sweep * f1;
        if (i > 0) a0 -= 0.8f; /* 段间重叠消接缝缺口 */
        if (i < seg - 1) a1 += 0.8f;
        dsc.start_angle = a0;
        dsc.end_angle = a1;
        lv_draw_arc(layer, &dsc);
    }
}

/* ---------------- 状态栏药丸（P1 后台露一手） ---------------- */
static void sync_statusbar(void)
{
    if (g_root || !g_running) {
        ui_statusbar_set_app_status(NULL, 0);
        return;
    }
    int mm = g_remaining / 60;
    int ss = g_remaining % 60;
    char buf[24];
    snprintf(buf, sizeof(buf), LV_SYMBOL_BELL " %02d:%02d", mm, ss);
    ui_statusbar_set_app_status(buf, phase_accent());
}

/* ---------------- 设置弹窗（时长三段可调） ---------------- */
typedef struct {
    lv_obj_t *mask, *val[3];
    int mins[3];
} cfg_panel_t;
static cfg_panel_t g_cfg_ui;

static void cfg_close(void)
{
    if (g_cfg_ui.mask) {
        lv_obj_delete(g_cfg_ui.mask);
        g_cfg_ui.mask = NULL;
    }
}

static void cfg_close_cb(lv_event_t *e)
{
    (void)e;
    ui_sound_click();
    cfg_close();
}

static void cfg_step_cb(lv_event_t *e)
{
    /* user_data 打包：高 8 位=行号，低 8 位=步进（+1/-1） */
    int tag = (int)(intptr_t)lv_event_get_user_data(e);
    int row = (tag >> 8) & 0xFF, step = (int8_t)(tag & 0xFF);
    static const int LO[3] = { 5, 1, 5 };
    static const int HI[3] = { 90, 30, 40 };
    int v = g_cfg_ui.mins[row] + step;
    if (v < LO[row]) v = LO[row];
    if (v > HI[row]) v = HI[row];
    g_cfg_ui.mins[row] = v;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(g_cfg_ui.val[row], buf);
    ui_sound_click();
}

static void cfg_save_cb(lv_event_t *e)
{
    (void)e;
    ui_sound_click();
    g_cfg_focus = g_cfg_ui.mins[0];
    g_cfg_short = g_cfg_ui.mins[1];
    g_cfg_long = g_cfg_ui.mins[2];
    cfg_write_to_stats();
    stats_request_save();
    /* 改时长后当前阶段回满并暂停（避免剩余>新总长的怪环） */
    g_running = false;
    g_just_done = false;
    g_remaining = phase_total();
    cfg_close();
    update_ui(true);
}

static void cfg_open_cb(lv_event_t *e)
{
    (void)e;
    ui_sound_click();
    if (g_cfg_ui.mask) return;
    g_cfg_ui.mins[0] = g_cfg_focus;
    g_cfg_ui.mins[1] = g_cfg_short;
    g_cfg_ui.mins[2] = g_cfg_long;

    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mask, 480, 480);
    lv_obj_align(mask, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x05050A), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mask, cfg_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_set_size(card, 340, 300);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(POMO_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A2A55), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = ui_label(card, "时长设置", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);

    static const char *rows[3] = { "专注", "短休息", "长休息" };
    for (int i = 0; i < 3; i++) {
        int y = 56 + i * 62;
        lv_obj_t *rl = ui_label(card, rows[i], &lv_font_cn_14, UI_TEXT);
        lv_obj_align(rl, LV_ALIGN_TOP_LEFT, 24, y + 6);

        lv_obj_t *minus = lv_button_create(card);
        lv_obj_set_size(minus, 40, 36);
        lv_obj_align(minus, LV_ALIGN_TOP_LEFT, 96, y);
        lv_obj_set_style_bg_color(minus, lv_color_hex(POMO_CARD2), 0);
        lv_obj_set_style_radius(minus, 10, 0);
        ui_label(minus, LV_SYMBOL_MINUS, &lv_font_montserrat_14, UI_TEXT);
        int tag = (i << 8) | (uint8_t)(int8_t)-1;
        lv_obj_add_event_cb(minus, cfg_step_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)tag);

        char vb[12];
        snprintf(vb, sizeof(vb), "%d", g_cfg_ui.mins[i]);
        g_cfg_ui.val[i] = ui_label(card, vb, &lv_font_montserrat_20,
                                   lv_color_hex(POMO_ORANGE));
        lv_obj_set_width(g_cfg_ui.val[i], 64);
        lv_obj_set_style_text_align(g_cfg_ui.val[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(g_cfg_ui.val[i], LV_ALIGN_TOP_LEFT, 144, y + 2);

        lv_obj_t *plus = lv_button_create(card);
        lv_obj_set_size(plus, 40, 36);
        lv_obj_align(plus, LV_ALIGN_TOP_LEFT, 216, y);
        lv_obj_set_style_bg_color(plus, lv_color_hex(POMO_CARD2), 0);
        lv_obj_set_style_radius(plus, 10, 0);
        ui_label(plus, LV_SYMBOL_PLUS, &lv_font_montserrat_14, UI_TEXT);
        lv_obj_add_event_cb(plus, cfg_step_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((i << 8) | 1));

        lv_obj_t *unit = ui_label(card, "分钟", &lv_font_cn_12,
                                  lv_color_hex(POMO_DIM));
        lv_obj_align(unit, LV_ALIGN_TOP_LEFT, 264, y + 8);
    }

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 120, 44);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 28, -16);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(POMO_CARD2), 0);
    lv_obj_set_style_radius(cancel, 22, 0);
    ui_label(cancel, "取消", &lv_font_cn_14, UI_TEXT);
    lv_obj_add_event_cb(cancel, cfg_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save = lv_button_create(card);
    lv_obj_set_size(save, 120, 44);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -28, -16);
    lv_obj_set_style_bg_color(save, lv_color_hex(POMO_RED), 0);
    lv_obj_set_style_radius(save, 22, 0);
    ui_label(save, "保存", &lv_font_cn_14, lv_color_white());
    lv_obj_add_event_cb(save, cfg_save_cb, LV_EVENT_CLICKED, NULL);

    g_cfg_ui.mask = mask;
}

/* ---------------- 主界面 ---------------- */
static void update_ui(bool phase_changed)
{
    sync_statusbar();
    /* 后台计时：页面已退出，只记录渲染进度，不碰任何页面控件 */
    if (!g_root) {
        g_shown_sec = g_remaining;
        return;
    }

    if (phase_changed) {
        /* 分段控件选中态：渐变胶囊 + 白字；未选中透明底 + 灰字 */
        static const int order[3] = { PH_FOCUS, PH_SHORT, PH_LONG };
        for (int i = 0; i < 3; i++) {
            bool sel = order[i] == g_phase;
            lv_obj_t *b = g_seg[i];
            if (!b) continue;
            if (sel) {
                uint32_t c0, c1;
                phase_colors(&c0, &c1); /* 注意：分段用的是当前阶段色 */
                lv_obj_set_style_bg_color(b, lv_color_hex(c0), 0);
                lv_obj_set_style_bg_grad_color(b, lv_color_hex(c1), 0);
                lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, 0);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(b, lv_color_hex(0x000000), 0);
            } else {
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_text_color(b, lv_color_hex(POMO_DIM), 0);
            }
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "—  %s  —", g_just_done ? "完成！" : phase_title());
        lv_label_set_text(g_sub_lbl, buf);
        lv_image_set_src(g_mascot, phase_mascot());
    }

    /* 逐位固定宽度渲染：Montserrat 数字是比例字宽，整串刷新会左右抖动 */
    int mm = g_remaining / 60;
    int ss = g_remaining % 60;
    set_digit(0, (char)('0' + mm / 10));
    set_digit(1, (char)('0' + mm % 10));
    set_digit(2, (char)('0' + ss / 10));
    set_digit(3, (char)('0' + ss % 10));

    /* 环形进度 = 剩余比例（满圈开始，随倒计时收短） */
    int total = phase_total();
    lv_arc_set_angles(g_arc, 0, total > 0 ? 360 * g_remaining / total : 0);
    lv_obj_invalidate(g_arc); /* 自绘渐变随角度/透明度重画 */

    pomo_stats_t st;
    stats_read(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), LV_SYMBOL_OK " 今日已专注 %d 次 · 共 %d 分钟",
             st.today, st.today_min);
    lv_label_set_text(g_stats_lbl, buf);

    lv_label_set_text(g_lbl_play, g_running
                      ? LV_SYMBOL_PAUSE " 暂停"
                      : LV_SYMBOL_PLAY " 开始");
    g_shown_sec = g_remaining;
}

static void next_phase(bool completed)
{
    if (completed) {
        ui_sound_alert(); /* 倒计时归零：三连滴（后台计时同样要响） */
        g_just_done = true; /* 完成态吉祥物 + 环呼吸，等用户开启下一段 */
        if (g_phase == PH_FOCUS) {
            stats_add_focus(phase_total() / 60);
            g_streak++;
            if (g_streak >= CYCLE_LEN) { /* 4 轮专注 → 长休息 */
                g_phase = PH_LONG;
            } else {
                g_phase = PH_SHORT;
            }
        } else if (g_phase == PH_LONG) { /* 长休息结束 → 新一轮 */
            g_streak = 0;
            g_phase = PH_FOCUS;
        } else {
            g_phase = PH_FOCUS;
        }
        g_running = false; /* 完成后停表：呼吸态等开启（用户 2026-11-17 设计语义） */
    } else { /* 手动切：分段控件接管（见 seg_cb）；此分支仅回归钩子保留 */
        if (g_phase == PH_LONG) g_streak = 0;
        g_phase = (g_phase == PH_FOCUS) ? PH_SHORT : PH_FOCUS;
        g_just_done = false;
    }
    g_remaining = phase_total();
    if (g_running) g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
    update_ui(true);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    /* 完成呼吸：不占新 timer，复用 200ms tick（正弦 1s 周期） */
    if (!g_running && g_just_done && g_root) {
        uint32_t ms = lv_tick_get() % 1000;
        int opa = 165 + (int)(80.0f * (0.5f + 0.5f * sinf(ms / 1000.0f * 6.28318f)));
        if (opa != g_ring_opa) {
            g_ring_opa = opa;
            lv_obj_invalidate(g_arc);
        }
        return;
    }
    if (!g_running) return;
    int sec = remaining_now();
    if (sec != g_shown_sec) {
        g_remaining = sec;
        update_ui(false);
    }
    if (sec == 0) next_phase(true);
}

static void start_pause_cb(lv_event_t *e)
{
    (void)e;
    ui_sound_click();
    if (g_running) {
        g_remaining = remaining_now();
        g_running = false;
        g_ring_opa = 154; /* 暂停压暗（设计稿：暂停=圆环降低亮度） */
    } else {
        if (g_just_done || g_remaining <= 0) {
            g_remaining = phase_total(); /* 呼吸态点开始 → 新阶段满时长 */
        }
        g_just_done = false;
        g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
        g_running = true;
        g_ring_opa = 255;
    }
    update_ui(true);
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    ui_sound_click();
    g_running = false;
    g_just_done = false;
    g_ring_opa = 255;
    g_remaining = phase_total();
    update_ui(true);
}

/* 模式分段控件：手动切换（不计数；离开长休息=本轮结束） */
static void seg_cb(lv_event_t *e)
{
    int want = (int)(intptr_t)lv_event_get_user_data(e);
    if (want == g_phase) return;
    ui_sound_click();
    if (g_phase == PH_LONG) g_streak = 0;
    g_phase = want;
    g_running = false;
    g_just_done = false;
    g_ring_opa = 255;
    g_remaining = phase_total();
    update_ui(true);
}

/* 背景暗红装饰圆（低透明度，静态） */
static void deco_dot(lv_obj_t *scr, int x, int y, int d)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_size(c, d, d);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x3A1520), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
}

/* 后台计时回归钩子（仅模拟器环境变量触发） */
static void demo_back_cb(lv_timer_t *t)
{
    (void)t;
    app_manager_back();
}

static void demo_reopen_cb(lv_timer_t *t)
{
    (void)t;
    app_manager_open(UI_APP_POMODORO);
}

void ui_pomodoro_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    g_root = scr;
    g_sub_lbl = g_stats_lbl = g_arc = NULL;
    g_mascot = g_gear = NULL;
    g_btn_play = g_lbl_play = NULL;
    memset(g_digits, 0, sizeof(g_digits));
    memset(g_seg, 0, sizeof(g_seg));

    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(POMO_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_gesture_back_add(scr);

    deco_dot(scr, 28, 96, 88);
    deco_dot(scr, 372, 150, 64);
    deco_dot(scr, 352, 336, 76);

    /* 顶部标题栏：番茄红点 + 标题 + 设置齿轮（圆屏安全区 x∈[126,354]） */
    lv_obj_t *dot = lv_obj_create(scr);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 148, 36);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(POMO_RED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_t *title = ui_label(scr, "番茄时钟", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 168, 30);
    /* 设置入口：无框小按钮（避免 deprecated 的 add_flag 用法） */
    g_gear = lv_button_create(scr);
    lv_obj_set_size(g_gear, 40, 40);
    lv_obj_align(g_gear, LV_ALIGN_TOP_LEFT, 306, 22);
    lv_obj_set_style_bg_opa(g_gear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_gear, 0, 0);
    lv_obj_set_style_shadow_width(g_gear, 0, 0);
    lv_obj_t *gl = ui_label(g_gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_16,
                            lv_color_hex(POMO_DIM));
    lv_obj_center(gl);
    lv_obj_add_event_cb(g_gear, cfg_open_cb, LV_EVENT_CLICKED, NULL);

    /* 模式分段控件：300×36 胶囊，三等分（点选切换，见 seg_cb） */
    lv_obj_t *segbar = lv_obj_create(scr);
    lv_obj_set_size(segbar, 300, 36);
    lv_obj_align(segbar, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_bg_color(segbar, lv_color_hex(POMO_CARD), 0);
    lv_obj_set_style_bg_opa(segbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(segbar, 0, 0);
    lv_obj_set_style_radius(segbar, 18, 0);
    lv_obj_clear_flag(segbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(segbar, 0, 0);
    static const char *segs[3] = { "专注", "休息", "长休" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(segbar);
        lv_obj_set_size(b, 98, 32);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 1 + i * 100, 0);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *l = ui_label(b, segs[i], &lv_font_cn_14, UI_TEXT);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, seg_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i == 0 ? PH_FOCUS
                                                  : (i == 1 ? PH_SHORT : PH_LONG)));
        g_seg[i] = b;
    }

    /* 中央渐变圆环 220px（indicator 透明，DRAW_POST 自绘渐变/呼吸）。
     * 环心 y=224：顶 114 与分段控件（74..110）留缝、底 334 与统计
     * 胶囊（340..）留缝——250px 版曾与分段控件叠字（首版实测）。 */
    g_arc = lv_arc_create(scr);
    lv_obj_set_size(g_arc, 220, 220);
    lv_obj_align(g_arc, LV_ALIGN_CENTER, 0, -16);
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(g_arc, 0, 0);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(0x1E1F2E), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g_arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_arc, 0, LV_PART_KNOB);
    lv_arc_set_rotation(g_arc, 270);
    lv_arc_set_bg_angles(g_arc, 0, 360);
    lv_arc_set_angles(g_arc, 0, 360);
    lv_obj_add_event_cb(g_arc, pomo_arc_draw_cb, LV_EVENT_DRAW_POST, NULL);

    /* 环内：吉祥物（64px RGB565，随模式/完成态切换）。y=132 让黑底
     * 方块完全离开描边（220px 环描边内缘 128，76px 版黑角曾切环）。 */
    g_mascot = lv_image_create(scr);
    lv_image_set_src(g_mascot, phase_mascot());
    lv_obj_align(g_mascot, LV_ALIGN_TOP_MID, 0, 132);

    /* 环内倒计时（56px 逐位定宽）：位置绝对稳定不抖 */
    static const lv_coord_t DXS[4] = { -60, -24, 24, 60 };
    for (int i = 0; i < 4; i++) {
        g_digits[i] = ui_label(scr, "0", &font_digits_56, UI_TEXT);
        lv_obj_set_width(g_digits[i], 40);
        lv_obj_set_style_text_align(g_digits[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(g_digits[i], LV_ALIGN_TOP_MID, DXS[i], 202);
    }
    lv_obj_t *colon = ui_label(scr, ":", &font_digits_56, UI_TEXT);
    lv_obj_set_width(colon, 18);
    lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 202);

    /* 环内副题：白色（用户 2026-11-17：灰紫在黑底上看不清） */
    g_sub_lbl = ui_label(scr, "", &lv_font_cn_14, UI_TEXT);
    lv_obj_align(g_sub_lbl, LV_ALIGN_TOP_MID, 0, 258);

    /* 今日统计胶囊（320×34） */
    lv_obj_t *stbar = lv_obj_create(scr);
    lv_obj_set_size(stbar, 320, 34);
    lv_obj_align(stbar, LV_ALIGN_TOP_MID, 0, 340);
    lv_obj_set_style_bg_color(stbar, lv_color_hex(POMO_CARD), 0);
    lv_obj_set_style_bg_opa(stbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stbar, 0, 0);
    lv_obj_set_style_radius(stbar, 17, 0);
    lv_obj_clear_flag(stbar, LV_OBJ_FLAG_SCROLLABLE);
    g_stats_lbl = ui_label(stbar, "", &lv_font_cn_14, UI_TEXT);
    lv_obj_center(g_stats_lbl);

    /* 底部双钮：重置（暗） + 开始/暂停（橙红渐变主钮） */
    lv_obj_t *b_reset = lv_button_create(scr);
    lv_obj_set_size(b_reset, 150, 52);
    lv_obj_align(b_reset, LV_ALIGN_TOP_MID, -82, 394);
    lv_obj_set_style_bg_color(b_reset, lv_color_hex(POMO_CARD2), 0);
    lv_obj_set_style_radius(b_reset, 26, 0);
    lv_obj_set_style_border_width(b_reset, 0, 0);
    lv_obj_set_style_shadow_width(b_reset, 0, 0);
    lv_obj_t *rl = ui_label(b_reset, LV_SYMBOL_REFRESH " 重置", &lv_font_cn_14, UI_TEXT);
    lv_obj_center(rl);
    lv_obj_add_event_cb(b_reset, reset_cb, LV_EVENT_CLICKED, NULL);

    g_btn_play = lv_button_create(scr);
    lv_obj_set_size(g_btn_play, 150, 52);
    lv_obj_align(g_btn_play, LV_ALIGN_TOP_MID, 82, 394);
    lv_obj_set_style_bg_color(g_btn_play, lv_color_hex(POMO_ORANGE), 0);
    lv_obj_set_style_bg_grad_color(g_btn_play, lv_color_hex(POMO_RED), 0);
    lv_obj_set_style_bg_grad_dir(g_btn_play, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_radius(g_btn_play, 26, 0);
    lv_obj_set_style_border_width(g_btn_play, 0, 0);
    lv_obj_set_style_shadow_width(g_btn_play, 0, 0);
    /* 主钮文字黑色（橙红渐变上白字对比不够，用户 2026-11-17） */
    g_lbl_play = ui_label(g_btn_play, LV_SYMBOL_PLAY " 开始",
                          &lv_font_cn_14, lv_color_hex(0x000000));
    lv_obj_center(g_lbl_play);
    lv_obj_add_event_cb(g_btn_play, start_pause_cb, LV_EVENT_CLICKED, NULL);

    /* 后台计时器只建一次（退出页面不销毁，ADR-011） */
    if (!g_timer) g_timer = lv_timer_create(tick_cb, TICK_MS, NULL);

    /* 统计+配置首次进入时异步加载（TF 不在/读失败则用内存默认值） */
    if (!g_stats_loaded && !g_stats_loading) {
        g_stats_loading = true;
        if (!start_thread(stats_load_worker, NULL, 8 * 1024)) {
            g_stats_loading = false;
            g_stats_loaded = true;
        }
    }

    update_ui(true);

    /* 回归钩子：GADGET_DEMO_POMO=1 启动即开始（headless 截帧验证运行态）；
     * =N（<60）把当前阶段剩余设为 N 秒；GADGET_DEMO_POMO_STREAK=n 预置
     * 本轮已完成数（验证长休息）；GADGET_DEMO_POMO_BG/REOPEN=<ms> 验证
     * 后台计时/状态恢复。只在首次进入生效。 */
    if (!g_demo_used) {
        g_demo_used = true;
        const char *demo = getenv("GADGET_DEMO_POMO");
        const char *demo_streak = getenv("GADGET_DEMO_POMO_STREAK");
        if (demo_streak) {
            g_streak = atoi(demo_streak);
            if (g_streak < 0 || g_streak >= CYCLE_LEN) g_streak = 0;
        }
        if (demo) {
            int s = atoi(demo);
            if (s > 0 && s < 60) {
                g_remaining = s;
                g_total_override = s;
            }
            g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
            g_running = true;
            g_ring_opa = 255;
            update_ui(false);
        }
        const char *demo_bg = getenv("GADGET_DEMO_POMO_BG");
        if (demo_bg) {
            lv_timer_t *t = lv_timer_create(demo_back_cb, (uint32_t)atoi(demo_bg), NULL);
            if (t) lv_timer_set_repeat_count(t, 1);
        }
        const char *demo_re = getenv("GADGET_DEMO_POMO_REOPEN");
        if (demo_re) {
            lv_timer_t *t = lv_timer_create(demo_reopen_cb, (uint32_t)atoi(demo_re), NULL);
            if (t) lv_timer_set_repeat_count(t, 1);
        }
    }
}

void ui_pomodoro_on_exit(void)
{
    /* 后台计时：不清计时器、不改运行状态；只断 UI 引用（在途异步回调
     * 靠 g_root==NULL 丢弃）。设置弹窗若开着一并收掉。 */
    cfg_close();
    g_root = g_sub_lbl = g_stats_lbl = g_arc = NULL;
    g_mascot = g_gear = NULL;
    g_btn_play = g_lbl_play = NULL;
    memset(g_digits, 0, sizeof(g_digits));
    memset(g_seg, 0, sizeof(g_seg));
    g_shown_sec = -1;
    sync_statusbar();
}
