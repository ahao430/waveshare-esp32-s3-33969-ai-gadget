/* ui_app_pages — 向 app_manager 注册全部 App 页面。
 * 额度页（Zhipu/Kimi）：环形 5h 额度 + 周额度条 + 重置时间 + 手动刷新；
 * 数据一律来自 data_source（mock 或 Bridge，ADR-001）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_launcher.h"
#include "ui_theme.h"

/* ---------------- 公共入口 ---------------- */

static void register_apps(void);

void ui_init(void)
{
    /* 数据源先于 UI 初始化：环境变量决定 mock/bridge（ADR-001） */
    ds_init(NULL, NULL, NULL);

    /* 顺序敏感：先注册 App（Launcher 网格按注册表构建），再建桌面 */
    register_apps();
    lv_obj_t *home = ui_launcher_screen();
    app_manager_set_home(home);
    lv_screen_load(home); /* Launcher 是启动屏幕（常驻系统 UI） */

    /* 演示/测试钩子：GADGET_DEMO_APP=<id> 启动即进入指定 App */
    const char *demo = getenv("GADGET_DEMO_APP");
    if (demo && demo[0]) {
        ui_open_app(atoi(demo));
    }
}

void ui_show_launcher(void) { app_manager_back(); }

void ui_open_app(int app_id) { app_manager_open(app_id); }

/* ---------------- 通用小部件 ---------------- */

static void back_cb(lv_event_t *e)
{
    (void)e;
    app_manager_back();
}

static lv_obj_t *topbar_build(lv_obj_t *scr, const char *title)
{
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(back_icon);

    return ui_label(scr, title, &lv_font_montserrat_20, UI_TEXT);
    /* title 由调用方 align */
}

/* ---------------- 额度页 ---------------- */

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *arc_lbl;
    lv_obj_t *arc_sub;
    lv_obj_t *week_bar;
    lv_obj_t *week_lbl;
    lv_obj_t *reset_lbl;
    lv_obj_t *updated_lbl;
    lv_obj_t *title_lbl;
    lv_obj_t *plan_lbl;
    lv_obj_t *dots[8];
    int sub_idx;   /* 当前显示的订阅 */
} quota_refs_t;

/* 同一时刻只有一个 App 屏幕存活（PLAN §4.3），单组静态引用足够 */
static quota_refs_t g_q;

static void quota_update(int app_id)
{
    (void)app_id;
    quota_refs_t *r = &g_q;
    const gadget_state_t *s = ds_state();
    if (r->sub_idx >= s->sub_count) r->sub_idx = 0;
    const ds_subscription_t *sub = &s->subs[r->sub_idx];

    char buf[96], tbuf[20];

    lv_label_set_text(r->title_lbl, sub->name);
    lv_label_set_text(r->plan_lbl, sub->plan);

    lv_arc_set_value(r->arc, sub->used_5h);
    lv_obj_set_style_arc_color(r->arc, ui_pct_color(sub->used_5h), LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "%d%%", sub->used_5h);
    lv_label_set_text(r->arc_lbl, buf);

    ui_fmt_md_hhmm(sub->reset_week, tbuf, sizeof(tbuf));
    snprintf(buf, sizeof(buf), "本周已用 %d%%（重置 %s）", sub->used_week, tbuf);
    lv_label_set_text(r->week_lbl, buf);
    lv_bar_set_value(r->week_bar, sub->used_week, LV_ANIM_OFF);

    ui_fmt_hhmm(sub->reset_5h, tbuf, sizeof(tbuf));
    snprintf(buf, sizeof(buf), "5小时窗口重置于 %s", tbuf);
    lv_label_set_text(r->reset_lbl, buf);

    ui_fmt_hhmm(s->updated_unix, tbuf, sizeof(tbuf));
    snprintf(buf, sizeof(buf), "更新于 %s · 来源 %s", tbuf, ds_source_name());
    lv_label_set_text(r->updated_lbl, buf);

    /* 页点 */
    for (int i = 0; i < s->sub_count && i < 8; i++) {
        if (r->dots[i]) {
            lv_obj_set_style_bg_color(r->dots[i], i == r->sub_idx ? UI_ACCENT : UI_CARD, 0);
            lv_obj_set_size(r->dots[i], i == r->sub_idx ? 16 : 8, 8);
        }
    }
}

static void quota_switch_cb(lv_event_t *e)
{
    int dir = (int)(uintptr_t)lv_event_get_user_data(e); /* -1 / +1 */
    const gadget_state_t *s = ds_state();
    if (s->sub_count == 0) return;
    g_q.sub_idx = (g_q.sub_idx + dir + s->sub_count) % s->sub_count;
    quota_update(-1);
}

static void quota_refresh_cb(lv_event_t *e)
{
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);
    /* Phase 1 简化：同步刷新（bridge 模式阻塞 ≤1.5s），TODO 固件换异步 */
    ds_refresh();
    quota_update(app_id);
}

/* ---- 预热（PLAN §5.6：手动 + 二次确认 + 不承诺重置窗口） ----
 * 不用 lv_msgbox：其 API 在 LVGL 9.2 换代（create 只收 parent），
 * 自建轻量确认弹窗避免版本绑定。 */

static void prewarm_modal_close_cb(lv_event_t *e)
{
    lv_obj_t *modal = lv_event_get_user_data(e);
    lv_obj_delete(modal);
}

static void prewarm_modal_confirm_cb(lv_event_t *e)
{
    lv_obj_t *modal = lv_event_get_user_data(e);
    char msg[160];
    ds_prewarm_zhipu(msg, sizeof(msg));
    quota_update(UI_APP_ZHIPU);

    lv_obj_t *lbl = g_q.updated_lbl; /* 结果落在订阅页更新行 */
    if (lbl) {
        char buf[192];
        snprintf(buf, sizeof(buf), "预热：%s", msg);
        lv_label_set_text(lbl, buf);
    }
    lv_obj_delete(modal);
}

static void quota_prewarm_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *root = lv_layer_top();
    lv_obj_t *modal = lv_obj_create(root);
    lv_obj_set_size(modal, 380, 176);
    lv_obj_center(modal);
    ui_style_card(modal);
    lv_obj_set_style_bg_color(modal, UI_CARD, 0);
    lv_obj_set_style_radius(modal, 20, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label(modal, "预热确认", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *text = ui_label(
        modal, "发起一次轻量模型请求？\n预热不会重置 5 小时窗口。",
        &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(text, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *cancel = lv_button_create(modal);
    lv_obj_set_size(cancel, 120, 44);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 12, 10);
    ui_style_card(cancel);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, prewarm_modal_close_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t *cl = ui_label(cancel, "取消", &lv_font_cn_14, UI_TEXT);
    lv_obj_center(cl);

    lv_obj_t *confirm = lv_button_create(modal);
    lv_obj_set_size(confirm, 120, 44);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, -12, 10);
    ui_style_card(confirm);
    lv_obj_set_style_bg_color(confirm, UI_ACCENT, 0);
    lv_obj_set_style_radius(confirm, 14, 0);
    lv_obj_add_event_cb(confirm, prewarm_modal_confirm_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t *kl = ui_label(confirm, "确认预热", &lv_font_cn_14, UI_TEXT);
    lv_obj_center(kl);
}

static void quota_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    quota_refs_t *r = &g_q;
    memset(r, 0, sizeof(*r));

    /* 顶部：back + 左右切换 */
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 56, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(back);
    lv_obj_set_style_radius(back, 14, 0);
    app_manager_back_on_click(back);
    lv_obj_t *back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(back_icon);

    lv_obj_t *prev = lv_button_create(scr);
    lv_obj_set_size(prev, 44, 44);
    lv_obj_align(prev, LV_ALIGN_TOP_LEFT, 64, 0);
    ui_style_card(prev);
    lv_obj_set_style_radius(prev, 14, 0);
    lv_obj_add_event_cb(prev, quota_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)-1);
    lv_obj_t *pi = ui_label(prev, LV_SYMBOL_LEFT, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(pi);

    lv_obj_t *next = lv_button_create(scr);
    lv_obj_set_size(next, 44, 44);
    lv_obj_align(next, LV_ALIGN_TOP_RIGHT, 0, 0);
    ui_style_card(next);
    lv_obj_set_style_radius(next, 14, 0);
    lv_obj_add_event_cb(next, quota_switch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1);
    lv_obj_t *ni = ui_label(next, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(ni);

    r->title_lbl = ui_label(scr, "", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(r->title_lbl, LV_ALIGN_TOP_MID, 0, 10);

    r->plan_lbl = ui_label(scr, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->plan_lbl, LV_ALIGN_TOP_MID, 0, 42);

    /* 5h 环形进度 */
    r->arc = lv_arc_create(scr);
    lv_obj_set_size(r->arc, 210, 210);
    lv_obj_align(r->arc, LV_ALIGN_CENTER, 0, -30);
    lv_arc_set_rotation(r->arc, 270);
    lv_arc_set_bg_angles(r->arc, 0, 360);
    lv_arc_set_range(r->arc, 0, 100);
    lv_obj_remove_style(r->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(r->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(r->arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(r->arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(r->arc, UI_CARD, LV_PART_MAIN);

    r->arc_lbl = ui_label(r->arc, "--%", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(r->arc_lbl, LV_ALIGN_CENTER, 0, -6);
    r->arc_sub = ui_label(r->arc, "5小时窗口已用", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->arc_sub, LV_ALIGN_CENTER, 0, 32);

    /* 每周额度条 */
    r->week_lbl = ui_label(scr, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->week_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -108);
    r->week_bar = lv_bar_create(scr);
    lv_obj_set_size(r->week_bar, LV_PCT(100) - 16, 16);
    lv_obj_align(r->week_bar, LV_ALIGN_BOTTOM_MID, 0, -84);
    lv_bar_set_range(r->week_bar, 0, 100);
    lv_obj_set_style_bg_color(r->week_bar, UI_CARD, 0);
    lv_obj_set_style_bg_color(r->week_bar, UI_ACCENT2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r->week_bar, 8, 0);
    lv_obj_set_style_radius(r->week_bar, 8, LV_PART_INDICATOR);

    r->reset_lbl = ui_label(scr, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->reset_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -54);

    r->updated_lbl = ui_label(scr, "", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(r->updated_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -32);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, LV_PCT(100) - 16, 48);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 8);
    ui_style_card(btn);
    lv_obj_set_style_bg_color(btn, UI_ACCENT, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, quota_refresh_cb, LV_EVENT_CLICKED, NULL);
    /* symbol 与中文分字体：拆两个 label（cn 子集无 symbol 字形） */
    lv_obj_t *btn_sym = ui_label(btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(btn_sym, LV_ALIGN_CENTER, -34, 0);
    lv_obj_t *btn_txt = ui_label(btn, "刷新", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(btn_txt, LV_ALIGN_CENTER, 14, 0);

    /* 页点 */
    const gadget_state_t *s = ds_state();
    for (int i = 0; i < s->sub_count && i < 8; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UI_CARD, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, -((s->sub_count - 1) * 12) / 2 + i * 12, 62);
        r->dots[i] = dot;
    }

    quota_update(app_id);
}

/* ---------------- 占位页 ---------------- */

typedef struct {
    int app_id;
    const char *title;
    const char *phase;
} placeholder_meta_t;

static const placeholder_meta_t g_placeholders[] = {
    { UI_APP_ZCODE,   "ZCode 会话", "Phase 4（数据源待定）" },
    { UI_APP_WEATHER, "天气",       "Phase 5+（Bridge 天气源）" },
    { UI_APP_MUSIC,   "音乐播放",   "Phase 5+" },
};

static void placeholder_build(lv_obj_t *scr, int app_id)
{
    const placeholder_meta_t *meta = NULL;
    for (size_t i = 0; i < sizeof(g_placeholders) / sizeof(g_placeholders[0]); i++) {
        if (g_placeholders[i].app_id == app_id) { meta = &g_placeholders[i]; break; }
    }
    if (!meta) return;

    lv_obj_t *title = topbar_build(scr, meta->title);
    lv_obj_set_style_text_font(title, &lv_font_cn_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LV_PCT(100) - 16, 200);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 20);
    ui_style_card(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = ui_label(card, LV_SYMBOL_WARNING, &lv_font_montserrat_28, UI_TEXT_DIM);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *l1 = ui_label(card, "暂未实现", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(l1, LV_ALIGN_CENTER, 0, 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "规划：%s", meta->phase);
    lv_obj_t *l2 = ui_label(card, buf, &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(l2, LV_ALIGN_CENTER, 0, 28);
}

/* ---------------- 注册表 ---------------- */

static void register_apps(void)
{
    const app_def_t defs[] = {
        { UI_APP_ZHIPU,    "订阅额度",   LV_SYMBOL_CHARGE,        true,  quota_build,           NULL },
        { UI_APP_MONITOR,  "服务器监控", LV_SYMBOL_EYE_OPEN,      true,  ui_monitor_build,      NULL },
        { UI_APP_ZCODE,    "ZCode",      LV_SYMBOL_LIST,          true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_CHAT,     "大肥鱼",     LV_SYMBOL_EDIT,          true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_MUSE,     "Muse",       LV_SYMBOL_AUDIO,         true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_XIAOZHI,  "小智",       LV_SYMBOL_VOLUME_MID,    true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_MUSIC,    "音乐播放",   LV_SYMBOL_SD_CARD,       false, placeholder_build,     NULL },
        { UI_APP_SETTINGS, "系统设置",   LV_SYMBOL_SETTINGS,      true,  ui_settings_build,     ui_settings_on_exit },
        { UI_APP_WEATHER,  "天气",       LV_SYMBOL_IMAGE,         false, placeholder_build,     NULL },
    };
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        if (!app_manager_register(&defs[i])) {
            fprintf(stderr, "app_manager_register failed at %zu\n", i);
        }
    }
}
