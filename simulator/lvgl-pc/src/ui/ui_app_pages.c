/* ui_app_pages — 向 app_manager 注册全部 App 页面。
 * 订阅页：横滑范式（ADR-008）——每订阅一 tile + 底部页点，无返回按钮
 * （上滑返回）、无 prev/next（左右滑动）。倒计时 1s 刷新。
 * 数据一律来自 data_source（mock 或 Bridge，ADR-001）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_manager.h"
#include "data_source.h"
#include "ui.h"
#include "ui_gesture.h"
#include "ui_keys.h"
#include "ui_launcher.h"
#include "ui_statusbar.h"
#include "ui_theme.h"
#include "voice_session.h"

/* ---------------- 公共入口 ---------------- */

static void register_apps(void);
static void voice_demo_begin_cb(lv_timer_t *t);
static void voice_demo_end_cb(lv_timer_t *t);
static void quota_on_exit(void);
static void quota_countdown_cb(lv_timer_t *t);
static void quota_update_all(void);
static void quota_tile_scroll_cb(lv_event_t *e);
static void quota_refresh_cb(lv_event_t *e);

void ui_init(void)
{
    /* 数据源先于 UI 初始化：环境变量决定 mock/bridge（ADR-001） */
    ds_init(NULL, NULL, NULL);

    /* 主题先于任何 UI 构建（UI_ACCENT 变量生效）；bridge 模式顺手
     * 拉一次自定义背景缓存（无则回退内置波浪） */
    int web_theme = -1;
    if (ds_appearance_fetch_theme(&web_theme) == 0 && web_theme >= 0) {
        ds_config_set_theme(web_theme);
    }
    ui_theme_select(ds_state()->theme_idx);
    (void)ds_appearance_fetch("simulator/lvgl-pc/assets/custom-bg.png");

    /* Web 下发按键映射（三键短按 + KEY3 长按；长按电源/BOOT 固件固定） */
    {
        char kp[8], kb[8], k3s[8], k3l[8];
        if (ds_keys_fetch(kp, kb, k3s, k3l, (int)sizeof(kp)) == 0) {
            ui_keys_apply_config(kp, kb, k3s, k3l);
        }
    }

    /* 顺序敏感：先注册 App（Launcher 网格按注册表构建），再建桌面 */
    register_apps();
    lv_obj_t *home = ui_launcher_screen();
    app_manager_set_home(home);
    lv_screen_load(home);

    /* 全局状态栏 overlay（PLAN §13.3） */
    ui_statusbar_init();

    /* 演示/测试钩子：GADGET_DEMO_APP=<id> 启动即进入指定 App */
    const char *demo = getenv("GADGET_DEMO_APP");
    if (demo && demo[0]) {
        ui_open_app(atoi(demo));
    }

    /* 语音链路自动化回归钩子：GADGET_DEMO_VOICE=1 */
    if (getenv("GADGET_DEMO_VOICE") && getenv("GADGET_DEMO_VOICE")[0]) {
        lv_timer_create(voice_demo_begin_cb, 1000, NULL);
    }

    /* 物理键自动化钩子：GADGET_DEMO_KEYS="pw@2000,boot@4000,..." */
    ui_keys_demo_init();
}

static void voice_demo_begin_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    vs_ptt_begin();
    lv_timer_create(voice_demo_end_cb, 4000, NULL);
}

static void voice_demo_end_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    vs_ptt_end();
    fprintf(stderr, "[voice-demo] ptt released\n");
}

void ui_show_launcher(void) { app_manager_back(); }
void ui_open_app(int app_id) { app_manager_open(app_id); }

/* ---------------- 订阅页（横滑范式） ---------------- */

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *arc_lbl;
    lv_obj_t *arc_sub;
    lv_obj_t *week_bar;
    lv_obj_t *week_val;
    lv_obj_t *week_reset;
    lv_obj_t *reset_lbl;
    lv_obj_t *updated_lbl;
    lv_obj_t *title_lbl;
    lv_obj_t *plan_lbl;
    int sub_idx;
} quota_refs_t;

/* 每订阅一 tile，refs 按下标隔离（单 App 屏幕存活，静态数组足够） */
static quota_refs_t g_q[DS_SUB_MAX];
static lv_obj_t *g_q_dots[UI_DOTS_MAX];
static lv_obj_t *g_q_tv;
static lv_timer_t *g_q_cd_timer;

static void quota_tile_scroll_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    int cur = ui_page_of_scroll(tv, 480);
    const gadget_state_t *s = ds_state();
    if (cur < 0) cur = 0;
    if (cur >= s->sub_count) cur = s->sub_count - 1;
    ui_dots_update(g_q_dots, s->sub_count, cur);
}

static void quota_update_one(int idx)
{
    quota_refs_t *r = &g_q[idx];
    const gadget_state_t *s = ds_state();
    if (idx >= s->sub_count) return;
    r->sub_idx = idx;
    const ds_subscription_t *sub = &s->subs[idx];

    char buf[96], tbuf[20];

    lv_label_set_text(r->title_lbl, sub->name);
    lv_label_set_text(r->plan_lbl, sub->plan);

    /* 环形显示剩余（5h 窗口） */
    int win_s = sub->win_5h_s > 0 ? sub->win_5h_s : 18000;
    int remain_5h_pct = 100 - sub->used_5h;
    lv_arc_set_value(r->arc, remain_5h_pct);
    lv_obj_set_style_arc_color(r->arc, ui_pct_color(sub->used_5h), LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "%.1fh", win_s * remain_5h_pct / 100.0 / 3600.0);
    lv_label_set_text(r->arc_lbl, buf);

    /* 本周额度卡 */
    int week_s = sub->week_s > 0 ? sub->week_s : 604800;
    snprintf(buf, sizeof(buf), "%.1f / %d h",
             week_s * sub->used_week / 100.0 / 3600.0, week_s / 3600);
    lv_label_set_text(r->week_val, buf);
    lv_bar_set_value(r->week_bar, sub->used_week, LV_ANIM_OFF);
    ui_fmt_md_hhmm(sub->reset_week, tbuf, sizeof(tbuf));
    snprintf(buf, sizeof(buf), "重置 %s", tbuf);
    lv_label_set_text(r->week_reset, buf);

    /* 倒计时行（QuotaFrame 借鉴） */
    ui_fmt_hhmm(sub->reset_5h, tbuf, sizeof(tbuf));
    long now = (long)time(NULL);
    long remain = sub->reset_5h > now ? sub->reset_5h - now : 0;
    snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld 后重置（%s）",
             remain / 3600, (remain % 3600) / 60, remain % 60, tbuf);
    lv_label_set_text(r->reset_lbl, buf);

    ui_fmt_hhmm(s->updated_unix, tbuf, sizeof(tbuf));
    snprintf(buf, sizeof(buf), "更新于 %s · 来源 %s", tbuf, ds_source_name());
    lv_label_set_text(r->updated_lbl, buf);
}

static void quota_update_all(void)
{
    const gadget_state_t *s = ds_state();
    for (int i = 0; i < s->sub_count && i < DS_SUB_MAX; i++) {
        quota_update_one(i);
    }
}

/* ---- 预热（PLAN §5.6：手动 + 二次确认 + 不承诺重置窗口） ---- */

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
    quota_update_all();

    lv_obj_t *lbl = g_q[0].updated_lbl; /* 结果落在订阅页更新行 */
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

/* 每条订阅一个 tile 的内容构造（480 宽 tile 内布局与原整页等价） */
static void quota_tile_build(lv_obj_t *tile, int idx)
{
    quota_refs_t *r = &g_q[idx];
    memset(r, 0, sizeof(*r));
    r->sub_idx = idx;

    /* 标题（tile 内顶部） */
    r->title_lbl = ui_label(tile, "", &lv_font_cn_20, UI_TEXT);
    lv_obj_align(r->title_lbl, LV_ALIGN_TOP_MID, 0, 10);
    r->plan_lbl = ui_label(tile, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->plan_lbl, LV_ALIGN_TOP_MID, 0, 42);

    /* 左：5h 剩余环 */
    r->arc = lv_arc_create(tile);
    lv_obj_set_size(r->arc, 200, 200);
    lv_obj_align(r->arc, LV_ALIGN_TOP_LEFT, 0, 112);
    lv_arc_set_rotation(r->arc, 270);
    lv_arc_set_bg_angles(r->arc, 0, 360);
    lv_arc_set_range(r->arc, 0, 100);
    lv_obj_remove_style(r->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(r->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(r->arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(r->arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(r->arc, UI_CARD, LV_PART_MAIN);

    r->arc_lbl = ui_label(r->arc, "--h", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(r->arc_lbl, LV_ALIGN_CENTER, 0, -8);
    r->arc_sub = ui_label(r->arc, "剩余 · 5小时窗口", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->arc_sub, LV_ALIGN_CENTER, 0, 34);

    /* 右列 1：本周额度卡 */
    lv_obj_t *wcard = lv_obj_create(tile);
    lv_obj_set_size(wcard, 232, 118);
    lv_obj_align(wcard, LV_ALIGN_TOP_RIGHT, 0, 106);
    ui_style_card(wcard);
    lv_obj_clear_flag(wcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *wt = ui_label(wcard, "本周额度", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(wt, LV_ALIGN_TOP_LEFT, 0, 0);
    r->week_val = ui_label(wcard, "", &lv_font_montserrat_20, UI_TEXT);
    lv_obj_align(r->week_val, LV_ALIGN_TOP_LEFT, 0, 22);
    r->week_bar = lv_bar_create(wcard);
    lv_obj_set_size(r->week_bar, 200, 10);
    lv_obj_align(r->week_bar, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_bar_set_range(r->week_bar, 0, 100);
    lv_obj_set_style_bg_color(r->week_bar, UI_CARD_HI, 0);
    lv_obj_set_style_bg_color(r->week_bar, UI_ACCENT2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r->week_bar, 5, 0);
    lv_obj_set_style_radius(r->week_bar, 5, LV_PART_INDICATOR);
    r->week_reset = ui_label(wcard, "", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(r->week_reset, LV_ALIGN_BOTTOM_LEFT, 0, -8);

    /* 右列 2：手动预热（二次确认见 prewarm 模态） */
    lv_obj_t *pw = lv_button_create(tile);
    lv_obj_set_size(pw, 232, 48);
    lv_obj_align(pw, LV_ALIGN_TOP_RIGHT, 0, 236);
    ui_style_card(pw);
    lv_obj_set_style_bg_color(pw, lv_color_hex(0xFF8A3D), 0);
    lv_obj_set_style_radius(pw, 14, 0);
    lv_obj_add_event_cb(pw, quota_prewarm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pw_sym = ui_label(pw, LV_SYMBOL_CHARGE, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(pw_sym, LV_ALIGN_CENTER, -44, 0);
    lv_obj_t *pw_txt = ui_label(pw, "手动预热", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(pw_txt, LV_ALIGN_CENTER, 12, 0);

    /* 右列 3：刷新 */
    lv_obj_t *btn = lv_button_create(tile);
    lv_obj_set_size(btn, 232, 44);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 0, 294);
    ui_style_card(btn);
    lv_obj_set_style_bg_color(btn, UI_CARD_HI, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_add_event_cb(btn, quota_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_sym = ui_label(btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(btn_sym, LV_ALIGN_CENTER, -40, 0);
    lv_obj_t *btn_txt = ui_label(btn, "刷新数据", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(btn_txt, LV_ALIGN_CENTER, 16, 0);

    /* 底部信息行 */
    r->reset_lbl = ui_label(tile, "", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->reset_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -46);
    r->updated_lbl = ui_label(tile, "", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(r->updated_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -24);
}

/* 订阅页装配：空态分支 + 每订阅一 tile（横滑 + 页点） */
static void quota_build(lv_obj_t *scr, int app_id)
{
    (void)app_id;
    ui_gesture_back_add(scr); /* 上滑返回（无返回按钮） */

    const gadget_state_t *s = ds_state();
    if (s->sub_count == 0) {
        lv_obj_t *card = lv_obj_create(scr);
        lv_obj_set_size(card, 320, 220);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 10);
        ui_style_card(card);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ic = ui_label(card, LV_SYMBOL_PLUS, &lv_font_montserrat_28, UI_ACCENT);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 28);
        lv_obj_t *t = ui_label(card, "还没有订阅", &lv_font_cn_16, UI_TEXT);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 74);
        lv_obj_t *d = ui_label(card, "请添加订阅", &lv_font_cn_14, UI_ACCENT);
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 106);
        lv_obj_t *h = ui_label(card, "在 Web 后台「订阅额度」页配置，\n设备端将自动显示",
                               &lv_font_cn_12, UI_TEXT_DIM);
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 140);
        lv_obj_t *btn = lv_button_create(scr);
        lv_obj_set_size(btn, 180, 44);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 130);
        ui_style_card(btn);
        lv_obj_set_style_bg_color(btn, UI_ACCENT, 0);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_add_event_cb(btn, quota_refresh_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)app_id);
        lv_obj_t *bs = ui_label(btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, UI_TEXT);
        lv_obj_align(bs, LV_ALIGN_CENTER, -30, 0);
        lv_obj_t *bt = ui_label(btn, "重新检查", &lv_font_cn_14, UI_TEXT);
        lv_obj_align(bt, LV_ALIGN_CENTER, 16, 0);
        return;
    }

    g_q_tv = lv_tileview_create(scr);
    lv_obj_set_size(g_q_tv, 480, 480);
    lv_obj_align(g_q_tv, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(g_q_tv, 0, 0);
    lv_obj_set_style_bg_opa(g_q_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_q_tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < s->sub_count && i < DS_SUB_MAX; i++) {
        lv_obj_t *tile = lv_tileview_add_tile(g_q_tv, i, 0, LV_DIR_HOR);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
        quota_tile_build(tile, i);
    }

    ui_dots_create(scr, g_q_dots, UI_DOTS_MAX, s->sub_count,
                   LV_ALIGN_BOTTOM_MID, -10);
    lv_obj_add_event_cb(g_q_tv, quota_tile_scroll_cb, LV_EVENT_SCROLL, NULL);
    quota_update_all();

    /* 倒计时 timer（更新全部 tile；on_exit 清理） */
    g_q_cd_timer = lv_timer_create(quota_countdown_cb, 1000, NULL);
}

/* 倒计时 tick：重算全部 tile 的 reset_lbl */
static void quota_countdown_cb(lv_timer_t *t)
{
    (void)t;
    const gadget_state_t *s = ds_state();
    for (int i = 0; i < s->sub_count && i < DS_SUB_MAX; i++) {
        quota_refs_t *r = &g_q[i];
        if (!r->reset_lbl) continue;
        const ds_subscription_t *sub = &s->subs[i];
        char buf[96], tbuf[20];
        ui_fmt_hhmm(sub->reset_5h, tbuf, sizeof(tbuf));
        long now = (long)time(NULL);
        long remain = sub->reset_5h > now ? sub->reset_5h - now : 0;
        snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld 后重置（%s）",
                 remain / 3600, (remain % 3600) / 60, remain % 60, tbuf);
        lv_label_set_text(r->reset_lbl, buf);
    }
}

static void quota_on_exit(void)
{
    if (g_q_cd_timer) {
        lv_timer_delete(g_q_cd_timer);
        g_q_cd_timer = NULL;
    }
    memset(g_q_dots, 0, sizeof(g_q_dots));
}

static void quota_refresh_cb(lv_event_t *e)
{
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);
    int before = ds_state()->sub_count;
    ds_refresh();
    if (ds_state()->sub_count != before) {
        app_manager_open(app_id); /* 订阅数量变化：重建（空态↔列表） */
        return;
    }
    quota_update_all();
}

/* ---------------- 占位页（上滑返回） ---------------- */

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
    ui_gesture_back_add(scr);
    const placeholder_meta_t *meta = NULL;
    for (size_t i = 0; i < sizeof(g_placeholders) / sizeof(g_placeholders[0]); i++) {
        if (g_placeholders[i].app_id == app_id) { meta = &g_placeholders[i]; break; }
    }
    if (!meta) return;

    lv_obj_t *title = ui_label(scr, meta->title, &lv_font_cn_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, LV_PCT(100), 200);
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
    /* 顺序 = 桌面分页顺序（一屏 4 个，横滑翻页）：
     * 页1 AI 三位 + 订阅｜页2 ZCode/天气/服务器/设置｜页3 音乐 */
    const app_def_t defs[] = {
        { UI_APP_CHAT,     "大肥鱼",  NULL,               0,        "A:simulator/lvgl-pc/assets/avatars/chat.png",    true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_XIAOZHI,  "小智",    NULL,               0,        "A:simulator/lvgl-pc/assets/avatars/xiaozhi.png", true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_MUSE,     "Muse",    NULL,               0,        "A:simulator/lvgl-pc/assets/avatars/muse.png",    true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_ZHIPU,    "订阅",    LV_SYMBOL_CHARGE,   0x2EC4B6, NULL,                                            true,  quota_build,           quota_on_exit },
        { UI_APP_ZCODE,    "ZCode",   LV_SYMBOL_KEYBOARD, 0x3A6FF7, NULL,                                            true,  ui_conversation_build, ui_conversation_on_exit },
        { UI_APP_WEATHER,  "天气",    LV_SYMBOL_TINT,     0x4F8CFF, NULL,                                            false, placeholder_build,     NULL },
        { UI_APP_MONITOR,  "服务器",  LV_SYMBOL_EYE_OPEN, 0x9B5DE5, NULL,                                            true,  ui_monitor_build,      NULL },
        { UI_APP_SETTINGS, "设置",    LV_SYMBOL_SETTINGS, 0x5C6B8A, NULL,                                            true,  ui_settings_build,     ui_settings_on_exit },
        { UI_APP_MUSIC,    "音乐",    LV_SYMBOL_AUDIO,    0xFF4D6D, NULL,                                            false, placeholder_build,     NULL },
    };
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        if (!app_manager_register(&defs[i])) {
            fprintf(stderr, "app_manager_register failed at %zu\n", i);
        }
    }
}
