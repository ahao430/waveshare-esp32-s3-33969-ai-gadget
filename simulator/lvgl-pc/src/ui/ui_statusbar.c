/* ui_statusbar — 实现，接口说明见 ui_statusbar.h。 */
#include <stdio.h>
#include <string.h>

#include "data_source.h"
#include "ui_theme.h"
#include "ui_statusbar.h"

#ifdef ESP_PLATFORM
#include "voice/audio_esp32.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#define VOL_APPLY(v) audio_esp32_set_volume(v)
/* AMOLED 亮度 = DCS 0x51 QSPI 事务。LVGL 回调（本文件全部调用点）本身
 * 在 LVGL 任务的显示锁内执行，无需再持锁（真机坑 #10）。 */
#define BRIGHT_APPLY(v) bsp_display_brightness_set(v)
#else
#define VOL_APPLY(v) ((void)0)
#define BRIGHT_APPLY(v) ((void)0)
#endif

static lv_obj_t *g_bar;
static lv_obj_t *g_right;      /* 右侧图标簇（flex 行，右对齐） */
static lv_obj_t *g_lbl_wifi;
static lv_obj_t *g_lbl_batt;   /* 电池五档图标 / USB 图标 */
static lv_obj_t *g_lbl_bolt;   /* 充电闪电（不充电时隐藏） */
static lv_obj_t *g_lbl_pct;    /* "86%"（无电池时显示 "USB"） */

static lv_timer_t *g_timer;

/* App 后台状态槽（P1）：单个 label 自带背景（不额外建容器，省一个
 * LVGL 对象）。只在调用方既有 timer 里更新，不新增任务/栈。 */
static lv_obj_t *g_lbl_app_status;

/* ---- 快速设置弹窗（点状态栏右侧图标簇）：上=音量，下=亮度 ---- */
static lv_obj_t *g_vol_pop;
static lv_obj_t *g_vol_lbl;
static lv_obj_t *g_bri_lbl;
static lv_timer_t *g_vol_close;

static void vol_apply(int v)
{
    ds_config_update(-1, v, -1);
    VOL_APPLY(v);
    ds_config_persist(); /* 防抖回写 Bridge（跨重启持久化） */
}

static void bright_apply(int v)
{
    if (v < 10) v = 10; /* 状态域下限（AMOLED 更低几乎全黑） */
    if (v > 100) v = 100;
    ds_config_update(v, -1, -1);
    BRIGHT_APPLY(v); /* 屏保恢复也从状态读，改完即生效 */
    ds_config_persist();
}

static void vol_close_cb(lv_timer_t *t)
{
    if (g_vol_pop) {
        lv_obj_delete(g_vol_pop);
        g_vol_pop = NULL;
        g_vol_lbl = NULL;
        g_bri_lbl = NULL;
    }
    g_vol_close = NULL;
    /* 自动关闭是一次性语义，必须删掉自己——原实现留成重复 timer，
     * 每开一次面板就攒一个 3s 僵尸；重开的新面板会被旧 timer 到期
     * 误关（用户反复使用后表现为"一打开就自动关闭"，2026-11-16）。 */
    lv_timer_delete(t);
}

static void vol_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    if (g_vol_lbl) lv_label_set_text(g_vol_lbl, buf);
    vol_apply(v);
    if (g_vol_close) lv_timer_reset(g_vol_close); /* 交互续命 */
}

static void bright_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    if (g_bri_lbl) lv_label_set_text(g_bri_lbl, buf);
    bright_apply(v);
    if (g_vol_close) lv_timer_reset(g_vol_close);
}

static void vol_popup_open(void);

static void vol_open_cb(lv_event_t *e)
{
    (void)e;
    vol_popup_open();
}

/* 对话页音量按钮也复用这个弹窗（ui.h 系外露） */
void ui_statusbar_volume_popup(void)
{
    vol_popup_open();
}

static void vol_popup_open(void)
{
    if (g_vol_pop) { /* 已开：只续命 */
        if (g_vol_close) lv_timer_reset(g_vol_close);
        return;
    }
    int cur = ds_state()->volume;
    int bri = ds_state()->brightness;

    g_vol_pop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_vol_pop, 300, 132);
    lv_obj_align(g_vol_pop, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(g_vol_pop, UI_CARD, 0);
    lv_obj_set_style_bg_opa(g_vol_pop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_vol_pop, UI_LINE, 0);
    lv_obj_set_style_border_width(g_vol_pop, 1, 0);
    lv_obj_set_style_radius(g_vol_pop, 16, 0);
    lv_obj_clear_flag(g_vol_pop, LV_OBJ_FLAG_SCROLLABLE);

    /* 上排：音量（y=-33 相对弹窗中心） */
    lv_obj_t *icon = ui_label(g_vol_pop,
                              cur == 0 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX,
                              &lv_font_montserrat_20, UI_ACCENT2);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, -33);

    g_vol_lbl = ui_label(g_vol_pop, "--%", &lv_font_montserrat_20, UI_TEXT);
    lv_obj_align(g_vol_lbl, LV_ALIGN_RIGHT_MID, -14, -33);

    lv_obj_t *sl = lv_slider_create(g_vol_pop);
    lv_obj_set_width(sl, 150);
    lv_obj_align(sl, LV_ALIGN_CENTER, 6, -33);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, cur, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, vol_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", cur);
    lv_label_set_text(g_vol_lbl, buf);

    /* 下排：屏幕亮度（y=+33；10~100，低于 10 近乎全黑不给选） */
    int b = bri;
    if (b < 10) b = 10;
    if (b > 100) b = 100;

    lv_obj_t *bicon = ui_label(g_vol_pop, LV_SYMBOL_EYE_OPEN,
                               &lv_font_montserrat_20, UI_ACCENT2);
    lv_obj_align(bicon, LV_ALIGN_LEFT_MID, 14, 33);

    g_bri_lbl = ui_label(g_vol_pop, "--%", &lv_font_montserrat_20, UI_TEXT);
    lv_obj_align(g_bri_lbl, LV_ALIGN_RIGHT_MID, -14, 33);

    lv_obj_t *bs = lv_slider_create(g_vol_pop);
    lv_obj_set_width(bs, 150);
    lv_obj_align(bs, LV_ALIGN_CENTER, 6, 33);
    lv_slider_set_range(bs, 10, 100);
    lv_slider_set_value(bs, b, LV_ANIM_OFF);
    lv_obj_add_event_cb(bs, bright_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    snprintf(buf, sizeof(buf), "%d%%", b);
    lv_label_set_text(g_bri_lbl, buf);

    g_vol_close = lv_timer_create(vol_close_cb, 3000, NULL);
}

/* 电量 → 五档图标（LVGL FontAwesome 电池组） */
static const char *batt_icon(int pct)
{
    if (pct >= 80) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60) return LV_SYMBOL_BATTERY_3;
    if (pct >= 40) return LV_SYMBOL_BATTERY_2;
    if (pct >= 20) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* 电量 → 颜色（20/10 阈值，PLAN §13.3 规则表） */
static lv_color_t batt_color(int pct)
{
    if (pct < 10) return UI_ERR;
    if (pct < 20) return UI_WARN;
    return UI_TEXT_DIM;
}

void ui_statusbar_apply_hw(void)
{
    const gadget_state_t *s = ds_state();
    VOL_APPLY(s->volume);
    BRIGHT_APPLY(s->brightness);
}

void ui_statusbar_refresh(void)
{
    if (!g_bar) return;
    const gadget_state_t *s = ds_state();

    /* 首次刷新：把（Web/默认）音量套到 codec、亮度套到面板——audio/
     * display init 早于数据源，只能等这里补（一次）。亮度默认 100 与
     * bsp_display_brightness_init(100) 对齐，此调用开机为 no-op；
     * 远程配置下发亮度/音量后再显式调 ui_statusbar_apply_hw。 */
    static bool vol_boot_applied;
    if (!vol_boot_applied) {
        vol_boot_applied = true;
        ui_statusbar_apply_hw();
    }

    lv_obj_set_style_text_color(g_lbl_wifi, s->wifi_ok ? UI_TEXT_DIM : UI_ERR, 0);

    if (!s->battery_present) {
        /* USB 供电、未检到电池：USB 图标 + "USB"，不显示虚假 100% */
        lv_label_set_text(g_lbl_batt, LV_SYMBOL_USB);
        lv_obj_set_style_text_color(g_lbl_batt, UI_TEXT_DIM, 0);
        lv_label_set_text(g_lbl_pct, s->usb_connected ? "USB" : "--");
        lv_obj_set_style_text_color(g_lbl_pct, UI_TEXT_DIM, 0);
        lv_obj_add_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(g_lbl_batt, batt_icon(s->battery_pct));
    lv_obj_set_style_text_color(g_lbl_batt, batt_color(s->battery_pct), 0);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", s->battery_pct);
    lv_label_set_text(g_lbl_pct, buf);
    lv_obj_set_style_text_color(g_lbl_pct, batt_color(s->battery_pct), 0);

    if (s->charging) {
        lv_obj_clear_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_statusbar_set_app_status(const char *text, uint32_t color)
{
    if (!g_bar) return;

    if (!text || !text[0]) {
        if (g_lbl_app_status) {
            lv_obj_delete(g_lbl_app_status);
            g_lbl_app_status = NULL;
        }
        return;
    }

    if (!g_lbl_app_status) {
        /* label 自带圆角背景 + 内边距 = 药丸；单对象，点击穿透（label
         * 默认不可点击）。内部分配失败时静默跳过（内部 RAM 紧张，
         * 状态指示不能反过来拖垮主界面）。 */
        g_lbl_app_status = lv_label_create(g_bar);
        if (!g_lbl_app_status) {
#ifdef ESP_PLATFORM
            ESP_LOGW("statusbar", "app slot alloc failed");
#endif
            return;
        }
        lv_label_set_text(g_lbl_app_status, text);
        lv_obj_set_style_text_font(g_lbl_app_status, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(g_lbl_app_status, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(g_lbl_app_status, lv_color_hex(0x0B1020), 0);
        lv_obj_set_style_bg_opa(g_lbl_app_status, LV_OPA_80, 0);
        lv_obj_set_style_radius(g_lbl_app_status, 12, 0);
        lv_obj_set_style_pad_hor(g_lbl_app_status, 10, 0);
        lv_obj_set_style_pad_ver(g_lbl_app_status, 4, 0);
        lv_obj_align(g_lbl_app_status, LV_ALIGN_TOP_MID, 0, 5);
#ifdef ESP_PLATFORM
        /* 一次性实测：药丸的 RAM 成本（P1 内存预算依据） */
        ESP_LOGI("statusbar", "app slot: heap=%u psram=%u largest_int=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#endif
        return;
    }

    if (strcmp(lv_label_get_text(g_lbl_app_status), text) != 0) {
        lv_label_set_text(g_lbl_app_status, text);
    }
    lv_obj_set_style_text_color(g_lbl_app_status, lv_color_hex(color), 0);
}

static void statusbar_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_statusbar_refresh();
}

void ui_statusbar_init(void)
{
    if (g_bar) return;

    g_bar = lv_obj_create(lv_layer_top());
    /* 真机目视反馈"状态栏小"：24→34px，图标 14→20、百分比 12→16
     *（百分比一直有，12px 在小屏看不清） */
    lv_obj_set_size(g_bar, 480, 34);
    lv_obj_align(g_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_bar, 0, 0);
    lv_obj_set_style_pad_all(g_bar, 0, 0);
    lv_obj_clear_flag(g_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_bar, LV_OBJ_FLAG_CLICKABLE); /* 点击穿透到下层屏幕 */

    /* 右侧图标簇：flex 行 [Wi-Fi][充电][电池][百分比]，整体右缩 20px。
     * 圆屏顶部安全区（同桌面时间卡左缩 10）：百分比原来贴 x=480 被圆角
     * 裁掉，右侧必须留白；电池与百分比只隔 4px 视为一组，充电图标在电池
     * 左侧（不充电时隐藏，flex 自动跳过不留空档）。
     * 固定宽 160：内容自适应宽会和 align 互相触发重排，且百分比位数变化
     * 会整簇抖动；固定盒 + MAIN_END 只让数字向左长。 */
    g_right = lv_obj_create(g_bar);
    lv_obj_set_size(g_right, 160, 28);
    lv_obj_align(g_right, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_opa(g_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_right, 0, 0);
    lv_obj_set_style_pad_all(g_right, 0, 0);
    lv_obj_clear_flag(g_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_right, LV_OBJ_FLAG_CLICKABLE); /* 点开快速音量 */
    lv_obj_add_event_cb(g_right, vol_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(g_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    g_lbl_wifi = ui_label(g_right, LV_SYMBOL_WIFI, &lv_font_montserrat_20, UI_TEXT_DIM);

    g_lbl_bolt = ui_label(g_right, LV_SYMBOL_CHARGE, &lv_font_montserrat_16, UI_OK);
    lv_obj_set_style_margin_left(g_lbl_bolt, 12, 0);
    lv_obj_add_flag(g_lbl_bolt, LV_OBJ_FLAG_HIDDEN);

    g_lbl_batt = ui_label(g_right, LV_SYMBOL_BATTERY_FULL, &lv_font_montserrat_20,
                          UI_TEXT_DIM);
    lv_obj_set_style_margin_left(g_lbl_batt, 12, 0);

    g_lbl_pct = ui_label(g_right, "--%", &lv_font_montserrat_16, UI_TEXT_DIM);
    /* -4：抵消两个字形盒的左右留白，让"电池 + 数字"看起来是一组 */
    lv_obj_set_style_margin_left(g_lbl_pct, -4, 0);

    g_timer = lv_timer_create(statusbar_timer_cb, 5000, NULL);
    (void)g_timer;
    ui_statusbar_refresh();
}
