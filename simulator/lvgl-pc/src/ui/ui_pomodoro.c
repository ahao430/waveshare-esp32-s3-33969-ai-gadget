/* ui_pomodoro — 番茄钟 App（2026-11-16 用户需求，v2 按评审意见增强）。
 *
 * 番茄工作法：25 分钟专注 → 5 分钟短休息；每完成 4 轮专注进入
 * 15 分钟长休息；自动循环。纯色扁平、96px 大号倒计时。
 *
 * 后台计时（ADR-011）：计时器是独立 lv_timer，不随页面销毁；退出 App
 * 时只清 UI 引用，倒计时继续跑（切回页面立即显示正确剩余）。阶段完成
 * 时把统计落盘。设备只有一个前台 App，但番茄钟属于"允许后台运行"的
 * 特例（与音乐同类）。
 *
 * 统计：`<TF>/data/pomodoro.json`（今日/累计完成数，按日期滚动）。
 * 文件 IO 一律 worker 线程 + lv_async_call 回 UI（踩坑 #11：LVGL 线程
 * 禁阻塞 IO），worker 栈 8KB；GADGET_SD_DIR 供模拟器回归。
 */
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
#include "ui.h"
#include "ui_gesture.h"
#include "ui_theme.h"

#define FOCUS_SEC (25 * 60)
#define SHORT_SEC (5 * 60)
#define LONG_SEC  (15 * 60)
#define CYCLE_LEN 4
#define TICK_MS   200

enum { PH_FOCUS = 0, PH_SHORT = 1, PH_LONG = 2 };

/* 纯色扁平配色：专注番茄红 / 短休息深绿 / 长休息深青；文字白色系 */
#define BG_FOCUS  0xB23A2E
#define BG_SHORT  0x1E6E55
#define BG_LONG   0x1F6079
#define INK_FOCUS 0x7E2419
#define INK_SHORT 0x14533F
#define INK_LONG  0x144A5E

static int g_phase = PH_FOCUS;
static int g_remaining = FOCUS_SEC; /* 剩余秒数（页面退出/重建不丢） */
static int g_streak;                /* 本轮已完成专注数（0..CYCLE_LEN-1） */
static int g_total_override;        /* 回归钩子：短时长演示覆盖阶段总长 */
static bool g_running;
static uint32_t g_deadline;         /* 运行中：剩余归零的 lv_tick */
static int g_shown_sec = -1;

static lv_timer_t *g_timer; /* 后台常驻（不随页面销毁，ADR-011） */
static lv_obj_t *g_root, *g_phase_lbl, *g_stats_lbl, *g_arc;
static lv_obj_t *g_btn_play, *g_lbl_play;
static lv_obj_t *g_digits[4]; /* 固定宽度逐位渲染：比例字体倒计时不抖 */
static bool g_demo_used;      /* 回归钩子只在首次进入时生效 */

/* ---------------- 统计（TF: data/pomodoro.json） ---------------- */
typedef struct {
    int today;
    int total;
    char date[11]; /* YYYY-MM-DD */
} pomo_stats_t;

static pomo_stats_t g_stats;
static bool g_stats_loaded, g_stats_loading, g_saving, g_save_pending;
static pthread_mutex_t g_stats_mu = PTHREAD_MUTEX_INITIALIZER;

static int phase_total(void)
{
    if (g_total_override > 0) return g_total_override;
    if (g_phase == PH_SHORT) return SHORT_SEC;
    if (g_phase == PH_LONG) return LONG_SEC;
    return FOCUS_SEC;
}

static uint32_t phase_bg(void)
{
    if (g_phase == PH_SHORT) return BG_SHORT;
    if (g_phase == PH_LONG) return BG_LONG;
    return BG_FOCUS;
}

static uint32_t phase_ink(void)
{
    if (g_phase == PH_SHORT) return INK_SHORT;
    if (g_phase == PH_LONG) return INK_LONG;
    return INK_FOCUS;
}

static const char *phase_title(void)
{
    if (g_phase == PH_SHORT) return "短休息 · 5 分钟";
    if (g_phase == PH_LONG) return "长休息 · 15 分钟";
    return "专注时间 · 25 分钟";
}

static void today_str(char out[11])
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    /* 先写足够大的缓冲再截断，避免 -Wformat-truncation（年份理论上可超 4 位） */
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

static void stats_read(int *today, int *total)
{
    pthread_mutex_lock(&g_stats_mu);
    *today = g_stats.today;
    *total = g_stats.total;
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
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                const cJSON *d = cJSON_GetObjectItem(root, "date");
                const cJSON *t = cJSON_GetObjectItem(root, "today");
                const cJSON *a = cJSON_GetObjectItem(root, "total");
                if (cJSON_IsString(d)) {
                    snprintf(st.date, sizeof(st.date), "%s", d->valuestring);
                }
                if (cJSON_IsNumber(t)) st.today = t->valueint;
                if (cJSON_IsNumber(a)) st.total = a->valueint;
                cJSON_Delete(root);
            }
        }
    }
    if (st.today < 0) st.today = 0;
    if (st.total < 0) st.total = 0;

    /* 合并而不是覆盖：load 期间若已完成专注（演示短时长可能发生），
     * 内存里的新增不能被磁盘旧值抹掉；今日数只在同一天时取较大值。 */
    char today[11];
    today_str(today);
    pthread_mutex_lock(&g_stats_mu);
    if (st.date[0] && strcmp(st.date, today) == 0 && st.today > g_stats.today) {
        g_stats.today = st.today;
    }
    if (st.total > g_stats.total) g_stats.total = st.total;
    if (g_stats.date[0] == '\0') {
        snprintf(g_stats.date, sizeof(g_stats.date), "%s", today);
    }
    pthread_mutex_unlock(&g_stats_mu);
    lv_async_call(stats_loaded_async, NULL);
    return NULL;
}

static void stats_add_focus(void)
{
    char today[11];
    today_str(today);
    pthread_mutex_lock(&g_stats_mu);
    if (strcmp(g_stats.date, today) != 0) {
        snprintf(g_stats.date, sizeof(g_stats.date), "%s", today);
        g_stats.today = 0;
    }
    g_stats.today++;
    g_stats.total++;
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

static void update_ui(bool phase_changed)
{
    /* 后台计时：页面已退出，只记录渲染进度，不碰任何 LVGL 对象 */
    if (!g_root) {
        g_shown_sec = g_remaining;
        return;
    }

    if (phase_changed) {
        lv_obj_set_style_bg_color(g_root, lv_color_hex(phase_bg()), 0);
        lv_obj_set_style_text_color(g_lbl_play, lv_color_hex(phase_ink()), 0);
        lv_label_set_text(g_phase_lbl, phase_title());
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

    int today, total_n;
    stats_read(&today, &total_n);
    char buf[64];
    snprintf(buf, sizeof(buf), "本轮 %d/%d · 今日 %d · 累计 %d",
             g_streak, CYCLE_LEN, today, total_n);
    lv_label_set_text(g_stats_lbl, buf);

    lv_label_set_text(g_lbl_play, g_running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    g_shown_sec = g_remaining;
}

static void next_phase(bool completed)
{
    if (completed) {
        if (g_phase == PH_FOCUS) {
            stats_add_focus();
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
    } else { /* 手动跳过：专注→短休息，休息→专注；不计完成数 */
        if (g_phase == PH_LONG) g_streak = 0; /* 跳过即结束本轮 */
        g_phase = (g_phase == PH_FOCUS) ? PH_SHORT : PH_FOCUS;
    }
    g_remaining = phase_total();
    if (g_running) g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
    update_ui(true);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
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
    if (g_running) {
        g_remaining = remaining_now();
        g_running = false;
    } else {
        if (g_remaining <= 0) g_remaining = phase_total();
        g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
        g_running = true;
    }
    update_ui(false);
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    g_running = false;
    g_remaining = phase_total();
    update_ui(false);
}

static void skip_cb(lv_event_t *e)
{
    (void)e;
    next_phase(false);
}

/* 白色半透明幽灵按钮（重置/跳过） */
static lv_obj_t *ghost_btn(lv_obj_t *scr, const char *txt, lv_coord_t x)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 80, 40);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -16);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_radius(b, 20, 0);
    ui_label(b, txt, &font_pomo_16, UI_TEXT);
    return b;
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
    g_phase_lbl = g_stats_lbl = g_arc = NULL;
    g_btn_play = g_lbl_play = NULL;
    memset(g_digits, 0, sizeof(g_digits));

    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(phase_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_gesture_back_add(scr);

    /* 大圆环进度：从 12 点方向顺时针，满圈开始随剩余时间收短 */
    g_arc = lv_arc_create(scr);
    lv_obj_set_size(g_arc, 340, 340);
    lv_obj_center(g_arc);
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(g_arc, 0, 0);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_arc, 0, LV_PART_KNOB);
    lv_arc_set_rotation(g_arc, 270);
    lv_arc_set_bg_angles(g_arc, 0, 360);
    lv_arc_set_angles(g_arc, 0, 360);

    /* 环内上方：阶段（加粗 20px，纯白） */
    g_phase_lbl = ui_label(scr, phase_title(), &font_pomo_20, UI_TEXT);
    lv_obj_align(g_phase_lbl, LV_ALIGN_TOP_MID, 0, 94);

    /* 逐位固定宽度倒计时（每格 60px，冒号 28px）：Montserrat 数字是
     * 比例字宽，整串刷新会左右抖；分格后位置绝对稳定。84px 字号保证
     * 数字不压到 340px 圆环（96px 实测压线）。 */
    static const lv_coord_t DXS[4] = { -104, -44, 44, 104 };
    for (int i = 0; i < 4; i++) {
        g_digits[i] = ui_label(scr, "0", &font_digits_84, UI_TEXT);
        lv_obj_set_width(g_digits[i], 60);
        lv_obj_set_style_text_align(g_digits[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(g_digits[i], LV_ALIGN_TOP_MID, DXS[i], 150);
    }
    lv_obj_t *colon = ui_label(scr, ":", &font_digits_84, UI_TEXT);
    lv_obj_set_width(colon, 28);
    lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 150);

    /* 环内中部：统计（加粗 16px，纯白） */
    g_stats_lbl = ui_label(scr, "", &font_pomo_16, UI_TEXT);
    lv_obj_align(g_stats_lbl, LV_ALIGN_TOP_MID, 0, 220);

    /* 环内下方：大号播放/暂停圆钮（白色实心，图标用阶段深色） */
    g_btn_play = lv_button_create(scr);
    lv_obj_set_size(g_btn_play, 120, 120);
    lv_obj_align(g_btn_play, LV_ALIGN_CENTER, 0, 72);
    lv_obj_set_style_radius(g_btn_play, 60, 0);
    lv_obj_set_style_bg_color(g_btn_play, lv_color_hex(0xFFF7F2), 0);
    lv_obj_set_style_border_width(g_btn_play, 0, 0);
    g_lbl_play = ui_label(g_btn_play, LV_SYMBOL_PLAY, &lv_font_montserrat_48,
                          lv_color_hex(phase_ink()));
    lv_obj_center(g_lbl_play);

    /* 环外底部：重置 / 跳过（圆屏安全区内，见布局注释） */
    lv_obj_t *b_reset = ghost_btn(scr, "重置", -45);
    lv_obj_add_event_cb(b_reset, reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b_skip = ghost_btn(scr, "跳过", 45);
    lv_obj_add_event_cb(b_skip, skip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_btn_play, start_pause_cb, LV_EVENT_CLICKED, NULL);

    /* 后台计时器只建一次（退出页面不销毁，ADR-011） */
    if (!g_timer) g_timer = lv_timer_create(tick_cb, TICK_MS, NULL);

    /* 统计首次进入时异步加载（TF 不在/读失败则用内存默认值） */
    if (!g_stats_loaded && !g_stats_loading) {
        g_stats_loading = true;
        if (!start_thread(stats_load_worker, NULL, 8 * 1024)) {
            g_stats_loading = false;
            g_stats_loaded = true;
        }
    }

    update_ui(true);

    /* 回归钩子：GADGET_DEMO_POMO=1 启动即开始（headless 截帧验证运行态）；
     * =N（<60）把当前阶段剩余设为 N 秒，用于验证阶段切换/完成计数；
     * GADGET_DEMO_POMO_STREAK=n 预置本轮已完成数（验证长休息）。
     * GADGET_DEMO_POMO_BG=<ms> 自动退回桌面、_REOPEN=<ms> 自动重开本 App
     * （验证后台计时/状态恢复）。只在首次进入生效，避免重开时再次触发。 */
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
                g_total_override = s; /* 环形进度与阶段时长同步缩短 */
            }
            g_deadline = lv_tick_get() + (uint32_t)g_remaining * 1000;
            g_running = true;
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
     * 靠 g_root==NULL 丢弃，不触碰已销毁控件）。统计 worker 同理。 */
    g_root = g_phase_lbl = g_stats_lbl = g_arc = NULL;
    g_btn_play = g_lbl_play = NULL;
    memset(g_digits, 0, sizeof(g_digits));
    g_shown_sec = -1;
}