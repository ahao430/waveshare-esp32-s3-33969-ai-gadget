/* ui_weather — 天气 App（PLAN §5；Bridge → Open-Meteo 免 Key）。
 * 布局：城市+更新时间 / 大温度+天气描述+体感湿度风 / 5 日预报横排 /
 * 右上刷新按钮。取数走 worker 线程（Bridge 冷启动可能 1~3s），LV 定时
 * 器轮询结果回填；10 分钟自动刷新；上滑返回（ui_gesture）。 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "data_source.h"
#include "ui.h"
#include "ui_gesture.h"
#include "ui_theme.h"
#include "ui_worker.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

/* 分类配色（desc 徽标 + 大温度） */
static uint32_t cat_color(const char *cat)
{
    if (strcmp(cat, "sun") == 0) return 0xFFB020;
    if (strcmp(cat, "rain") == 0) return 0x3EC4B6;
    if (strcmp(cat, "snow") == 0) return 0x9BD4F0;
    if (strcmp(cat, "thunder") == 0) return 0x9B5DE5;
    if (strcmp(cat, "fog") == 0) return 0x9AA5B1;
    return 0x8FA3C8; /* cloud/默认 */
}

/* ---- worker 取数（网络线程） ---- */
static ds_weather_t g_wx;
static volatile int g_wx_state = 0; /* 0=idle 1=fetching 2=ready 3=error */

static void *wx_worker(void *arg)
{
    (void)arg;
    g_wx_state = 1;
    ds_weather_t w;
    if (ds_weather_fetch(&w) == 0 && w.valid) {
        g_wx = w;
        g_wx_state = 2;
    } else {
        g_wx_state = 3;
    }
    return NULL;
}

static void wx_fetch_start(void)
{
    /* ESP32 worker 栈预算：HTTP 链（host/path/req/chunk≈5KB）+ cJSON
     * 递归 + 帧开销，给 16KB（栈上大缓冲已全部改为堆分配）。
     * 内部 RAM 碎片化时 16KB 会 ENOMEM → ui_worker_start 自动回落 12KB
     * （2026-11-16 实测：订阅 worker 同一问题导致页面永远空）。 */
    int rc = ui_worker_start(wx_worker, NULL, 16 * 1024);
#ifdef ESP_PLATFORM
    if (rc != 0) {
        ESP_LOGW("wx", "worker start failed rc=%d", rc);
    }
#else
    (void)rc;
#endif
}

/* ---- UI 引用 ---- */
static lv_obj_t *g_loc_lbl, *g_time_lbl, *g_temp_lbl, *g_desc_lbl, *g_meta_lbl;
static lv_obj_t *g_day_lbls[5][3]; /* 天名/描述/温度 */
static lv_obj_t *g_err_lbl;
static lv_timer_t *g_poll_timer, *g_auto_timer;
static time_t g_built_at;

/* 日期 → 标签（今天/明天/后天/周X） */
static void day_label(const char *date, char *out, int cap)
{
    int y, m, d;
    if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) {
        snprintf(out, (size_t)cap, "%s", date);
        return;
    }
    struct tm tm0 = {0};
    tm0.tm_year = y - 1900; tm0.tm_mon = m - 1; tm0.tm_mday = d;
    time_t t = timegm(&tm0);
    time_t now = time(NULL);
    int diff = (int)((t - (now / 86400) * 86400 + 43200) / 86400); // 与"今天"差几天
    static const char *wk[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (diff == 0) snprintf(out, (size_t)cap, "今天");
    else if (diff == 1) snprintf(out, (size_t)cap, "明天");
    else if (diff == 2) snprintf(out, (size_t)cap, "后天");
    else snprintf(out, (size_t)cap, "%s", wk[(4 + (int)(t / 86400)) % 7]);
}

static void wx_apply(void)
{
    char buf[64];
    lv_obj_set_style_text_color(g_temp_lbl, lv_color_hex(cat_color(g_wx.category)), 0);
    lv_label_set_text(g_loc_lbl, g_wx.location[0] ? g_wx.location : "定位中");
    snprintf(buf, sizeof(buf), "%d°", g_wx.temp_c);
    lv_label_set_text(g_temp_lbl, buf);
    lv_label_set_text(g_desc_lbl, g_wx.desc);
    snprintf(buf, sizeof(buf), "体感 %d° · 湿度 %d%% · 风 %d km/h",
             g_wx.feels_c, g_wx.humidity, g_wx.wind_kmh);
    lv_label_set_text(g_meta_lbl, buf);

    struct tm tmi;
    localtime_r(&g_built_at, &tmi);
    snprintf(buf, sizeof(buf), "%02d:%02d 更新", tmi.tm_hour, tmi.tm_min);
    lv_label_set_text(g_time_lbl, buf);

    for (int i = 0; i < 5; i++) {
        if (!g_day_lbls[i][0]) continue;
        if (i < g_wx.day_n) {
            char lab[10];
            day_label(g_wx.days[i].date, lab, sizeof(lab));
            lv_label_set_text(g_day_lbls[i][0], lab);
            lv_label_set_text(g_day_lbls[i][1], g_wx.days[i].desc);
            snprintf(buf, sizeof(buf), "%d° / %d°", g_wx.days[i].tmax, g_wx.days[i].tmin);
            lv_label_set_text(g_day_lbls[i][2], buf);
            lv_obj_clear_flag(g_day_lbls[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(g_day_lbls[i][1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(g_day_lbls[i][2], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_day_lbls[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_day_lbls[i][1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_day_lbls[i][2], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (g_err_lbl) lv_obj_add_flag(g_err_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void wx_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (g_wx_state == 2) {
        g_built_at = time(NULL);
        wx_apply();
        g_wx_state = 0;
    } else if (g_wx_state == 3) {
        if (g_err_lbl) lv_obj_clear_flag(g_err_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_loc_lbl, "获取失败");
        g_wx_state = 0;
    }
}

static void wx_auto_cb(lv_timer_t *t)
{
    (void)t;
    if (g_wx_state == 0) wx_fetch_start();
}

static void wx_refresh_cb(lv_event_t *e)
{
    (void)e;
    if (g_wx_state == 0) wx_fetch_start();
}

/* ---- 构建 ---- */
void ui_weather_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    ui_gesture_back_add(scr);

    g_loc_lbl = ui_label(scr, "…", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(g_loc_lbl, LV_ALIGN_TOP_MID, 0, 52);
    g_time_lbl = ui_label(scr, "", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(g_time_lbl, LV_ALIGN_TOP_MID, 0, 82);

    g_temp_lbl = ui_label(scr, "--°", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(g_temp_lbl, LV_ALIGN_TOP_MID, 0, 118);

    g_desc_lbl = ui_label(scr, "获取中…", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(g_desc_lbl, LV_ALIGN_TOP_MID, 0, 196);

    g_meta_lbl = ui_label(scr, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(g_meta_lbl, LV_ALIGN_TOP_MID, 0, 228);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 360, 2);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_style_bg_color(line, UI_CARD_HI, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 1, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    /* 5 日预报 */
    for (int i = 0; i < 5; i++) {
        int x = -168 + i * 84;
        g_day_lbls[i][0] = ui_label(scr, "—", &lv_font_cn_14, UI_TEXT);
        lv_obj_align(g_day_lbls[i][0], LV_ALIGN_TOP_MID, x, 300);
        g_day_lbls[i][1] = ui_label(scr, "—", &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(g_day_lbls[i][1], LV_ALIGN_TOP_MID, x, 328);
        g_day_lbls[i][2] = ui_label(scr, "—", &lv_font_cn_14, UI_TEXT);
        lv_obj_align(g_day_lbls[i][2], LV_ALIGN_TOP_MID, x, 354);
    }

    /* 刷新按钮（右上） */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 56, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -12, 46);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, UI_CARD, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, wx_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = ui_label(btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, UI_ACCENT);
    lv_obj_center(ic);

    g_err_lbl = ui_label(scr, "天气获取失败：Bridge 未连接或定位服务不可达\n点右上 ↻ 重试",
                         &lv_font_cn_14, UI_ERR);
    lv_obj_align(g_err_lbl, LV_ALIGN_CENTER, 0, 120);
    lv_obj_add_flag(g_err_lbl, LV_OBJ_FLAG_HIDDEN);

    g_built_at = time(NULL);
    g_wx_state = 0;
    g_poll_timer = lv_timer_create(wx_poll_cb, 200, NULL);
    g_auto_timer = lv_timer_create(wx_auto_cb, 10 * 60 * 1000, NULL);
    wx_fetch_start();
}

void ui_weather_on_exit(void)
{
    if (g_poll_timer) { lv_timer_delete(g_poll_timer); g_poll_timer = NULL; }
    if (g_auto_timer) { lv_timer_delete(g_auto_timer); g_auto_timer = NULL; }
    memset(g_day_lbls, 0, sizeof(g_day_lbls));
    g_err_lbl = NULL;
}
