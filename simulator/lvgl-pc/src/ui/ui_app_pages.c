/* ui_app_pages — 向 app_manager 注册全部 App 页面。
 * 订阅页：横滑范式（ADR-008）——每订阅一 tile + 底部页点，无返回按钮
 * （上滑返回）、无 prev/next（左右滑动）。倒计时 1s 刷新。
 * 数据一律来自 data_source（mock 或 Bridge，ADR-001）。
 */
#include <pthread.h>
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
#include "ui_worker.h"
#include "voice_session.h"

/* ---------------- 公共入口 ---------------- */

/* 启动阶段留痕：UI 构建路径上任何一步炸栈/死机都要能定位到步骤 */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define UILOG(fmt, ...) ESP_LOGI("ui", fmt, ##__VA_ARGS__)
#else
#define UILOG(fmt, ...) ((void)0)
#endif

static void register_apps(void);
static void register_apps_layout(const char *layout_csv, int lay_n);
/* 真机：开机时 Wi-Fi 未起，register_apps 不拉 Web 排布（回落出厂），
 * GOT_IP 后由 ui_remote_config_prefetch 置位再补拉（12KB 大栈任务里跑） */
#ifdef ESP_PLATFORM
static bool s_remote_layout_ready = false;
#else
#define s_remote_layout_ready true
#endif

/* 远端配置两段式：prefetch 只做网络/文件（不持显示锁，HTTP 超时最坏
 * 数秒——曾把抓取放锁内，联网初期桌面冻屏 ~8s 且 probe 全程 BUSY）；
 * refresh 持锁应用结果（改主题/按键/重建桌面，毫秒级）。
 * 由 main 在 IP_EVENT_STA_GOT_IP 之后用独立任务先后调用：
 *  - 不能在事件回调里跑（回调栈 4KB，http_client 需要 4KB 缓冲）
 *  - 不能在 ui_init 里跑（Wi-Fi 还没起，请求必失败）
 *  - 调用方保证 refresh 已持显示锁 */
static struct {
    bool done;
    bool have_theme;
    int theme;
    bool have_keys;
    char kp[8], kb[8], k3s[8], k3l[8];
    bool have_layout;
    char layout[192];
    int lay_n;
    bool have_devcfg;
    ds_device_config_t devcfg; /* 订阅间隔 + 亮度/音量（设备回写值回放） */
} s_rcfg;

/* 开机请求纪律（用户 2026-11-16 拍板）：进主页只做 Wi-Fi 连接 + 远程
 * 配置（主题/按键/设备配置/排布），**不做任何业务数据预热**——订阅
 * 数据打开订阅页时才拉（quota_fetch_start），小智直连 OTA 打开小智
 * App 时才发起（xz_probe_start，Stage 2 接入）。曾试过"联网后预热
 * 订阅+写 TF 缓存"，因用户不希望开机有多余请求而移除。 */

#ifdef ESP_PLATFORM
/* 小智直连探测（ADR-014 Stage 1）：OTA 注册拿激活码；身份/端点持久化
 * NVS。由小智 App 打开时调用（Stage 2 接入 UI：激活码上屏 + 轮询）。 */
#include "xz_ota.h"

static void *xz_probe_worker(void *arg)
{
    (void)arg;
    xz_ota_result_t r;
    char err[96];
    if (xz_ota_request(&r, err, sizeof(err)) == 0) {
        UILOG("xz probe: bound=%d code=%s stack_free=%u", r.bound,
              r.activation_code, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    } else {
        UILOG("xz probe: fail %s", err);
    }
    return NULL;
}

static void xz_probe_start(void);

/* 启动失败（rc=12 ENOMEM）重试：内部 RAM 被并发 worker 挤满时错峰
 * 10s 再试（一次性 timer，LVGL 线程安全）。 */
static void xz_probe_retry_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    xz_probe_start();
}

static void xz_probe_start(void)
{
    static int tries;
    int rc = ui_worker_start(xz_probe_worker, NULL, 16 * 1024);
    UILOG("xz probe: worker rc=%d (try %d)", rc, tries + 1);
    if (rc != 0 && ++tries < 6) {
        lv_timer_t *t = lv_timer_create(xz_probe_retry_cb, 10000, NULL);
        if (t) lv_timer_set_repeat_count(t, 1);
    }
}
#else
static void xz_probe_start(void)
{
}
#endif

void ui_remote_config_prefetch(void)
{
    s_rcfg.have_theme =
        ds_appearance_fetch_theme(&s_rcfg.theme) == 0 && s_rcfg.theme >= 0;
    /* 背景图：HTTP 下载落文件（文件路径仅 UI 消费，无 LVGL 调用） */
    (void)ds_appearance_fetch(DS_CUSTOM_BG_PATH);
    s_rcfg.have_keys = ds_keys_fetch(s_rcfg.kp, s_rcfg.kb, s_rcfg.k3s, s_rcfg.k3l,
                                     (int)sizeof(s_rcfg.kp)) == 0;
    s_rcfg.have_devcfg = ds_device_config_fetch(&s_rcfg.devcfg) == 0;
#ifdef ESP_PLATFORM
    s_remote_layout_ready = true;
    s_rcfg.have_layout =
        ds_launcher_fetch(s_rcfg.layout, sizeof(s_rcfg.layout), &s_rcfg.lay_n) == 0 &&
        s_rcfg.lay_n > 0;
    /* 开机请求纪律：到此为止——业务数据（订阅/小智）打开对应 App 才拉 */
#else
    s_rcfg.have_layout = false; /* 模拟器 ui_init 已就地应用排布 */
#endif
    s_rcfg.done = true;
}

void ui_remote_config_refresh(void)
{
    if (!s_rcfg.done) ui_remote_config_prefetch(); /* 未预取（直调路径） */

    UILOG("remote cfg: theme=%d keys=%d devcfg=%d layout=%d",
          (int)s_rcfg.have_theme, (int)s_rcfg.have_keys,
          (int)s_rcfg.have_devcfg, (int)s_rcfg.have_layout);

    if (s_rcfg.have_theme) {
        ds_config_set_theme(s_rcfg.theme);
        ui_theme_select(ds_state()->theme_idx);
    }
    if (s_rcfg.have_keys) {
        ui_keys_apply_config(s_rcfg.kp, s_rcfg.kb, s_rcfg.k3s, s_rcfg.k3l);
    }
    if (s_rcfg.have_devcfg) {
        ds_config_set_refresh_interval(s_rcfg.devcfg.sub_refresh_interval_s);
        /* 亮度/音量 = 设备回写的持久化值（-1=云端没存过，保持本地默认）。
         * 此时状态栏开机套用已跑过，应用新值要显式调一次。 */
        if (s_rcfg.devcfg.brightness >= 10 || s_rcfg.devcfg.volume >= 0) {
            ds_config_update(s_rcfg.devcfg.brightness >= 10 ? s_rcfg.devcfg.brightness : -1,
                             s_rcfg.devcfg.volume >= 0 ? s_rcfg.devcfg.volume : -1,
                             -1);
            ui_statusbar_apply_hw();
            UILOG("remote cfg: bright=%d vol=%d", s_rcfg.devcfg.brightness,
                  s_rcfg.devcfg.volume);
        }
    }

    /* 主页排布：真机开机时 Wi-Fi 未起、拉不到 Web 配置（register_apps
     * 回落出厂顺序），联网后在这里用预取结果重排并重建桌面。 */
    if (s_rcfg.have_layout && !app_manager_in_app()) {
        register_apps_layout(s_rcfg.layout, s_rcfg.lay_n);
        ui_launcher_rebuild();
    }
}

static void voice_demo_begin_cb(lv_timer_t *t);
static void voice_demo_end_cb(lv_timer_t *t);
static void quota_on_exit(void);
static void quota_countdown_cb(lv_timer_t *t);
static void quota_auto_refresh_cb(lv_timer_t *t);
static void quota_update_all(void);
static int quota_fetch_start(int app_id);
static void quota_tile_scroll_cb(lv_event_t *e);
static void quota_refresh_cb(lv_event_t *e);

void ui_init(void)
{
    /* 数据源先于 UI 初始化：环境变量决定 mock/bridge（ADR-001） */
    ds_init(NULL, NULL, NULL);
    UILOG("init: ds ready (%s)", ds_source_name());

    /* 主题先于任何 UI 构建（UI_ACCENT 变量生效）。
     * 真机：此时 Wi-Fi 还没起，Bridge 拉不到，且 main 任务栈只有 3.5KB
     *（http_client 要 4KB 缓冲 → 栈溢出重启）。远端配置改由 main 在
     * GOT_IP 后调 ui_remote_config_refresh()（独立大栈任务）补齐。
     * 模拟器没有 Wi-Fi 事件，就地拉。 */
#ifndef ESP_PLATFORM
    {
        int web_theme = -1;
        if (ds_appearance_fetch_theme(&web_theme) == 0 && web_theme >= 0) {
            ds_config_set_theme(web_theme);
        }
        (void)ds_appearance_fetch(DS_CUSTOM_BG_PATH);
        char kp[8], kb[8], k3s[8], k3l[8];
        if (ds_keys_fetch(kp, kb, k3s, k3l, (int)sizeof(kp)) == 0) {
            ui_keys_apply_config(kp, kb, k3s, k3l);
        }
        ds_device_config_t dc;
        if (ds_device_config_fetch(&dc) == 0) {
            ds_config_set_refresh_interval(dc.sub_refresh_interval_s);
            if (dc.brightness >= 10 || dc.volume >= 0) {
                ds_config_update(dc.brightness >= 10 ? dc.brightness : -1,
                                 dc.volume >= 0 ? dc.volume : -1, -1);
                /* 此处状态栏尚未 init，首次刷新的开机套用会带新值 */
            }
        }
    }
#endif
    UILOG("init: theme select");
    ui_theme_select(ds_state()->theme_idx);
    UILOG("init: theme ok");

    /* 顺序敏感：先注册 App（Launcher 网格按注册表构建），再建桌面 */
    register_apps();
    UILOG("init: launcher build");
    lv_obj_t *home = ui_launcher_screen();
    UILOG("init: launcher built");
    app_manager_set_home(home);
    lv_screen_load(home);
    UILOG("init: screen loaded");

    /* 全局状态栏 overlay（PLAN §13.3） */
    ui_statusbar_init();
    UILOG("init: statusbar ok");

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
    lv_obj_t *pct_sign;  /* % 号（小号字，跟在 84px 数字右侧） */
    lv_obj_t *arc_sub;   /* 环内副标：剩余额度/账户余额 */
    lv_obj_t *cd_big;    /* 重置倒计时大数字（设计稿：环外 48px） */
    lv_obj_t *cd_cap;    /* 倒计时说明行：下次重置 · 5 小时周期 */
    lv_obj_t *week_cap;  /* "周额度" 静态标签 */
    lv_obj_t *week_val;  /* "42%" 大号百分比 */
    lv_obj_t *week_bar;
    lv_obj_t *week_right; /* "剩余" 静态标签 */
    lv_obj_t *updated_lbl;
    lv_obj_t *title_lbl;
    lv_obj_t *plan_lbl;
    lv_obj_t *refresh_lbl; /* "刷新"按钮文字（拉取中转"刷新中…"） */
    int sub_idx;
    int arc_used; /* 当前 used%（-1=余额型不画弧；自绘渐变取色用） */
} quota_refs_t;

/* 每订阅一 tile，refs 按下标隔离（单 App 屏幕存活，静态数组足够） */
static quota_refs_t g_q[DS_SUB_MAX];
static lv_obj_t *g_q_dots[UI_DOTS_MAX];
static lv_obj_t *g_q_tv;
static lv_timer_t *g_q_cd_timer;
static lv_timer_t *g_q_auto_timer;
static int g_q_app_id;

/* 霓虹渐变弧（设计稿 2026-11-17）：LVGL 9.2 的 arc 不支持渐变描边
 * （源码无 arc_grad 属性），在 DRAW_POST 阶段自绘 24 段线性插值弧叠在
 * 透明化的 indicator 上。正常=主题双色（accent2→accent），剩余<20%=橙，
 * 耗尽=红。只在数值变化时随 invalidate 重画（每 60s 一次），无逐帧开销。 */
static void quota_arc_draw_cb(lv_event_t *e)
{
    quota_refs_t *r = lv_event_get_user_data(e);
    if (!r || r->arc_used < 0) return;
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_color_t c0, c1;
    if (r->arc_used >= 100) {       /* 耗尽：红渐变 */
        c0 = lv_color_hex(0xFF6B6B);
        c1 = lv_color_hex(0xB92E2E);
    } else if (r->arc_used >= 80) { /* 剩余 <20%：橙渐变 */
        c0 = lv_color_hex(0xFFC14D);
        c1 = lv_color_hex(0xFF7A3D);
    } else {                        /* 正常：主题双色 */
        c0 = UI_ACCENT2;
        c1 = UI_ACCENT;
    }

    int remain = 100 - r->arc_used;
    if (remain <= 0) return; /* 空弧不画 */

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.width = 18;
    dsc.rounded = 1;
    dsc.opa = LV_OPA_COVER;
    dsc.center.x = (coords.x1 + coords.x2) / 2;
    dsc.center.y = (coords.y1 + coords.y2) / 2;
    dsc.radius = (coords.x2 - coords.x1) / 2;

    const int seg = 24;
    float sweep = 360.0f * (float)remain / 100.0f;
    for (int i = 0; i < seg; i++) {
        float f0 = (float)i / seg, f1 = (float)(i + 1) / seg;
        float mid = (f0 + f1) / 2;
        dsc.color = lv_color_mix(c1, c0, (uint8_t)(255 * mid));
        dsc.start_angle = 270.0f + sweep * f0;
        dsc.end_angle = 270.0f + sweep * f1;
        lv_draw_arc(layer, &dsc);
    }
}

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
    /* 套餐行拆成 header：左=服务商名（title_lbl），右徽章=到期部分。
     * Bridge 的 plan 形如 "GLM Coding Lite · 12月11日到期"，取 · 后半段 */
    const char *expiry = sub->plan;
    const char *dot = strstr(sub->plan, "·");
    if (dot) {
        expiry = dot + strlen("·");
        while (*expiry == ' ') expiry++;
    }
    lv_label_set_text(r->plan_lbl, expiry[0] ? expiry : "--");

    /* 元素级显隐（按 provider 实际返回的窗口，不补默认值）：
     *   5h 窗口有 → 环+剩余百分比+5h 倒计时；
     *   周窗口有 → 周额度行（智谱 v1 套餐无周限，不得显示假 168h 条）；
     *   两者皆无 → 余额型（按量计费，中心显示余额）。
     * 只有周限的套餐：环改显本周剩余百分比+周倒计时。 */
    bool has_5h = sub->win_5h_s > 0;
    bool has_week = sub->week_s > 0;
    long now = (long)time(NULL);

    if (has_5h || has_week) {
        lv_obj_clear_flag(r->arc, LV_OBJ_FLAG_HIDDEN);
        int used_pct = has_5h ? sub->used_5h : sub->used_week;
        int remain_pct = 100 - used_pct;
        if (remain_pct < 0) remain_pct = 0;
        r->arc_used = used_pct; /* 自绘渐变取色（quota_arc_draw_cb） */
        lv_arc_set_value(r->arc, remain_pct);
        lv_obj_invalidate(r->arc);
        /* 巨型数字：digits 字体无 % 字形 → % 用小号独立标签跟在右侧 */
        lv_obj_set_style_text_font(r->arc_lbl, &font_digits_84, 0);
        snprintf(buf, sizeof(buf), "%d", remain_pct);
        lv_label_set_text(r->arc_lbl, buf);
        lv_obj_align(r->arc_lbl, LV_ALIGN_TOP_MID, 0, 119);
        lv_obj_clear_flag(r->pct_sign, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(lv_obj_get_parent(r->arc_lbl));
        int nw = lv_obj_get_width(r->arc_lbl);
        int px = nw / 2 + 22;
        if (px > 90) px = 90; /* "100" 时不越出环内缘 */
        lv_obj_align(r->pct_sign, LV_ALIGN_TOP_MID, px, 147);
        lv_label_set_text(r->arc_sub, "剩余额度");
        lv_obj_align(r->arc_sub, LV_ALIGN_TOP_MID, 0, 207);
        /* 环外大倒计时（cd_big 由 1s timer 持续刷新，此处先落初值） */
        long reset_at = has_5h ? sub->reset_5h : sub->reset_week;
        long remain = reset_at > now ? reset_at - now : 0;
        snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld",
                 remain / 3600, (remain % 3600) / 60, remain % 60);
        lv_label_set_text(r->cd_big, buf);
        lv_obj_clear_flag(r->cd_big, LV_OBJ_FLAG_HIDDEN);
        ui_fmt_hhmm(s->updated_unix, tbuf, sizeof(tbuf));
        snprintf(buf, sizeof(buf), "下次重置 · %s · %s%s",
                 has_5h ? "5 小时周期" : "本周额度",
                 s->subs_cache ? "离线 " : "更新于 ", tbuf);
        lv_label_set_text(r->cd_cap, buf);
        lv_obj_clear_flag(r->cd_cap, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r->arc, LV_OBJ_FLAG_HIDDEN);
        r->arc_used = -1;
        lv_obj_set_style_text_font(r->arc_lbl, &lv_font_montserrat_48, 0);
        lv_label_set_text(r->arc_lbl, sub->balance[0] ? sub->balance : "--");
        lv_obj_align(r->arc_lbl, LV_ALIGN_TOP_MID, 0, 150);
        lv_obj_add_flag(r->pct_sign, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(r->arc_sub, "账户余额");
        lv_obj_align(r->arc_sub, LV_ALIGN_TOP_MID, 0, 214);
        lv_obj_add_flag(r->cd_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->cd_cap, LV_OBJ_FLAG_HIDDEN);
    }

    if (has_week) {
        lv_obj_clear_flag(r->week_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(r->week_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(r->week_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(r->week_right, LV_OBJ_FLAG_HIDDEN);
        int wremain = 100 - sub->used_week;
        if (wremain < 0) wremain = 0;
        snprintf(buf, sizeof(buf), "%d%%", wremain);
        lv_label_set_text(r->week_val, buf);
        lv_obj_update_layout(lv_obj_get_parent(r->week_val));
        int vw = lv_obj_get_width(r->week_val);
        lv_obj_align(r->week_val, LV_ALIGN_TOP_MID, -120 + vw / 2, 346);
        lv_bar_set_value(r->week_bar, wremain, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(r->week_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->week_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->week_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->week_right, LV_OBJ_FLAG_HIDDEN);
    }

    }

/* 说明行已含更新时间（见上），不再有独立 updated 行 */

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
    lv_obj_t *mask = lv_event_get_user_data(e);
    char msg[256];
    ds_prewarm_zhipu(msg, sizeof(msg));
    quota_update_all();
    lv_obj_delete(mask);

    /* 结果弹窗：冷却/失败/额度变化都醒目可见（原来只写卡片 12px 小字） */
    lv_obj_t *m2 = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m2, 480, 480);
    lv_obj_align(m2, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(m2, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m2, LV_OPA_50, 0);
    lv_obj_set_style_border_width(m2, 0, 0);
    lv_obj_clear_flag(m2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m2, prewarm_modal_close_cb, LV_EVENT_CLICKED, m2);

    lv_obj_t *card = lv_obj_create(m2);
    lv_obj_set_size(card, 380, 200);
    lv_obj_center(card);
    ui_style_card(card);
    lv_obj_set_style_bg_color(card, UI_CARD, 0);
    lv_obj_set_style_radius(card, 20, 0);

    lv_obj_t *title = ui_label(card, "预热结果", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *body = ui_label(card, msg, &lv_font_cn_14, UI_TEXT);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = ui_label(card, "点击任意处关闭", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* 预热（PLAN §5.6）：智谱 Coding Plan 专用端点实证（2026-11-17）——
 * Bridge 走 /api/coding/paas/v4 发一次 glm-5.3 的 "hi"（max_tokens=1），
 * 消耗约 1% 5h 额度并启动窗口（实测 100%→99%，reset_at 从 None 变为
 * 5 小时后）。普通 /api/paas/v4 端点不计套餐（429/零消耗），已在
 * Bridge 端修正。确认弹窗说明消耗与窗口语义。 */
static void quota_prewarm_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mask, 480, 480);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mask, prewarm_modal_close_cb, LV_EVENT_CLICKED, mask);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_set_size(card, 380, 200);
    lv_obj_center(card);
    ui_style_card(card);
    lv_obj_set_style_bg_color(card, UI_CARD, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label(card, "预热确认", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *text = ui_label(
        card,
        "发送一次轻量请求（glm-5.3）\n"
        "将消耗约 1% 额度并启动 5 小时窗口",
        &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(text, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 120, 44);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 12, 10);
    ui_style_card(cancel);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, prewarm_modal_close_cb, LV_EVENT_CLICKED, mask);
    lv_obj_t *cl = ui_label(cancel, "取消", &lv_font_cn_14, UI_TEXT);
    lv_obj_center(cl);

    lv_obj_t *confirm = lv_button_create(card);
    lv_obj_set_size(confirm, 120, 44);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, -12, 10);
    ui_style_card(confirm);
    lv_obj_set_style_bg_color(confirm, UI_ACCENT, 0);
    lv_obj_set_style_radius(confirm, 14, 0);
    lv_obj_add_event_cb(confirm, prewarm_modal_confirm_cb, LV_EVENT_CLICKED, mask);
    lv_obj_t *kl = ui_label(confirm, "确认预热", &lv_font_cn_14, UI_TEXT);
    lv_obj_center(kl);
}

/* 每条订阅一个 tile 的内容构造。
 * 480 圆屏布局安全带：所有内容按 |dx|²+|dy|² ≤ 240² 收敛（四角不可见），
 * 故一律居中对齐、底部行收窄（旧的 BOTTOM_LEFT 两行会被圆角切掉）。 */
static void quota_tile_build(lv_obj_t *tile, int idx)
{
    quota_refs_t *r = &g_q[idx];
    memset(r, 0, sizeof(*r));
    r->sub_idx = idx;

    /* 顶部 header（设计稿：左套餐名 + 右到期徽章。圆屏无角落→flex 行
     * 整体居中，自动按内容宽度收敛） */
    lv_obj_t *hdr = lv_obj_create(tile);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, 26);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    r->title_lbl = ui_label(hdr, "", &lv_font_cn_16, UI_TEXT);
    lv_obj_t *chip = lv_obj_create(hdr);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_color(chip, UI_CARD_HI, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 12, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_hor(chip, 10, 0);
    lv_obj_set_style_pad_ver(chip, 2, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    r->plan_lbl = ui_label(chip, "--", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_center(r->plan_lbl);

    /* 居中大环（200px）：环心=巨型剩余百分比 + "剩余额度"副标 */
    r->arc = lv_arc_create(tile);
    lv_obj_set_size(r->arc, 200, 200);
    lv_obj_align(r->arc, LV_ALIGN_TOP_MID, 0, 72);
    lv_arc_set_rotation(r->arc, 270);
    lv_arc_set_bg_angles(r->arc, 0, 360);
    lv_arc_set_range(r->arc, 0, 100);
    lv_obj_remove_style(r->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(r->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(r->arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(r->arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(r->arc, lv_color_hex(0x333A55), LV_PART_MAIN); /* 设计稿灰紫 track */
    /* 渐变 indicator 改由 quota_arc_draw_cb 自绘（LVGL 9.2 arc 无渐变），
     * 默认 indicator 全透明防止叠色 */
    lv_obj_set_style_arc_opa(r->arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_add_event_cb(r->arc, quota_arc_draw_cb, LV_EVENT_DRAW_POST, r);

    /* 环心：巨型剩余百分比（84px digits）+ 小号 % + 副标（环 200 缩后居中） */
    r->arc_lbl = ui_label(tile, "0", &font_digits_84, UI_TEXT);
    lv_obj_align(r->arc_lbl, LV_ALIGN_TOP_MID, 0, 119);
    r->pct_sign = ui_label(tile, "%", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(r->pct_sign, LV_ALIGN_TOP_MID, 40, 147);
    r->arc_sub = ui_label(tile, "剩余额度", &lv_font_cn_16, UI_TEXT_DIM);
    lv_obj_align(r->arc_sub, LV_ALIGN_TOP_MID, 0, 207);

    /* 环外：重置倒计时（设计稿：48px 大数字 + 说明行；说明行含更新时间） */
    r->cd_big = ui_label(tile, "0:00:00", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(r->cd_big, LV_ALIGN_TOP_MID, 0, 276);
    r->cd_cap = ui_label(tile, "下次重置 · 5 小时周期", &lv_font_cn_12, UI_TEXT_DIM);
    lv_obj_align(r->cd_cap, LV_ALIGN_TOP_MID, 0, 328);

    /* 周额度行（设计稿：周额度 [42%] [进度条] 剩余；无周限整行隐藏） */
    r->week_cap = ui_label(tile, "周额度", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->week_cap, LV_ALIGN_TOP_MID, -150, 352);
    r->week_val = ui_label(tile, "--", &lv_font_montserrat_28, UI_ACCENT2);
    lv_obj_align(r->week_val, LV_ALIGN_TOP_MID, -90, 346);
    r->week_bar = lv_bar_create(tile);
    lv_obj_set_size(r->week_bar, 130, 10);
    lv_obj_align(r->week_bar, LV_ALIGN_TOP_MID, 24, 362);
    lv_bar_set_range(r->week_bar, 0, 100);
    lv_obj_set_style_bg_color(r->week_bar, UI_CARD_HI, 0);
    lv_obj_set_style_bg_color(r->week_bar, UI_ACCENT2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r->week_bar, 5, 0);
    lv_obj_set_style_radius(r->week_bar, 5, LV_PART_INDICATOR);
    r->week_right = ui_label(tile, "剩余", &lv_font_cn_14, UI_TEXT_DIM);
    lv_obj_align(r->week_right, LV_ALIGN_TOP_MID, 118, 352);

    /* 底部操作栏（设计稿：整条胶囊，左「刷新」| 竖线 | 右「手动预热」双行） */
    const gadget_state_t *s = ds_state();
    bool can_pre = idx < s->sub_count && s->subs[idx].can_prewarm;
    lv_obj_t *opbar = lv_obj_create(tile);
    lv_obj_set_size(opbar, 232, 46);
    lv_obj_align(opbar, LV_ALIGN_TOP_MID, 0, 390);
    lv_obj_set_style_bg_color(opbar, UI_CARD_HI, 0);
    lv_obj_set_style_bg_opa(opbar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(opbar, 23, 0);
    lv_obj_set_style_border_width(opbar, 0, 0);
    lv_obj_set_style_pad_all(opbar, 0, 0);
    lv_obj_clear_flag(opbar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左区：刷新（透明点击区） */
    lv_obj_t *zone_refresh = lv_obj_create(opbar);
    lv_obj_set_size(zone_refresh, can_pre ? 100 : 200, 46);
    lv_obj_align(zone_refresh, can_pre ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(zone_refresh, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zone_refresh, 0, 0);
    lv_obj_clear_flag(zone_refresh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(zone_refresh, quota_refresh_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)g_q_app_id);
    lv_obj_t *btn_sym = ui_label(zone_refresh, LV_SYMBOL_REFRESH,
                                 &lv_font_montserrat_16, UI_ACCENT2);
    lv_obj_align(btn_sym, LV_ALIGN_CENTER, can_pre ? -28 : -34, 0);
    r->refresh_lbl = ui_label(zone_refresh, "刷新", &lv_font_cn_16, UI_TEXT);
    lv_obj_align(r->refresh_lbl, LV_ALIGN_CENTER, can_pre ? 20 : 16, 0);

    if (can_pre) {
        /* 竖分隔线 */
        lv_obj_t *div = lv_obj_create(opbar);
        lv_obj_set_size(div, 1, 24);
        lv_obj_align(div, LV_ALIGN_CENTER, -16, 0);
        lv_obj_set_style_bg_color(div, UI_TEXT_DIM, 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_40, 0);
        lv_obj_set_style_border_width(div, 0, 0);
        lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);

        /* 右区：手动预热（单行大字，副标按用户要求去掉） */
        lv_obj_t *zone_pre = lv_obj_create(opbar);
        lv_obj_set_size(zone_pre, 132, 46);
        lv_obj_align(zone_pre, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_opa(zone_pre, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(zone_pre, 0, 0);
        lv_obj_clear_flag(zone_pre, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(zone_pre, quota_prewarm_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)idx);
        lv_obj_t *t1 = ui_label(zone_pre, "手动预热", &lv_font_cn_16,
                                lv_color_hex(0xFF9A4D));
        lv_obj_align(t1, LV_ALIGN_CENTER, 0, 0);
    }

    /* 更新时间并入 cd_cap 说明行（设计稿无独立行，底部空间让给页点） */
    (void)r->updated_lbl;
}

/* 订阅页装配：空态分支 + 每订阅一 tile（横滑 + 页点） */
static void quota_build(lv_obj_t *scr, int app_id)
{
    g_q_app_id = app_id;
    ui_gesture_back_add(scr); /* 上滑返回（无返回按钮） */

    /* 进页即异步拉一次（ds_state 不再惰性刷新后这里保证首屏不空；
     * 必须在空态分支之前——否则空 → 不拉 → 永远空）。
     * worker+lv_async_call：网络不通只影响本次数据，不冻 UI。 */
    quota_fetch_start(app_id);

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
    ui_dots_update(g_q_dots, s->sub_count, 0); /* 初始高亮 */
    lv_obj_add_event_cb(g_q_tv, quota_tile_scroll_cb, LV_EVENT_SCROLL, NULL);
    quota_update_all();

    /* 倒计时 timer（更新全部 tile；ADR-012 P2：登记后由 app_manager
     * 在切走时兜底删除，on_exit 只断引用） */
    g_q_cd_timer = app_manager_timer_create(quota_countdown_cb, 1000, NULL);

    /* 自动刷新 timer：间隔由 Web 下发（ds_config_refresh_interval，
     * 默认 900s）；每次进页面重建。改设置后重开页面或等下一次拉取
     * 即热生效（worker 顺带回拉配置，见 quota_fetch_done），无需重启 */
    {
        int ivs = ds_config_refresh_interval();
        if (ivs < 60) ivs = 900;
        g_q_auto_timer = app_manager_timer_create(quota_auto_refresh_cb,
                                                  (uint32_t)ivs * 1000u, NULL);
    }

    /* 回归钩子：GADGET_DEMO_SUB_IDX=<n> 直接定位到第 n 张订阅卡截帧 */
    const char *sid = getenv("GADGET_DEMO_SUB_IDX");
    if (sid && sid[0]) {
        int i = atoi(sid);
        if (i > 0 && i < s->sub_count && i < DS_SUB_MAX) {
            lv_obj_set_tile_id(g_q_tv, i, 0, LV_ANIM_OFF);
            ui_dots_update(g_q_dots, s->sub_count, i);
        }
    }
}

/* 倒计时 tick：重算窗口型 tile 的环外大倒计时（cd_big；按 5h 或周窗口） */
static void quota_countdown_cb(lv_timer_t *t)
{
    (void)t;
    const gadget_state_t *s = ds_state();
    long now = (long)time(NULL);
    for (int i = 0; i < s->sub_count && i < DS_SUB_MAX; i++) {
        quota_refs_t *r = &g_q[i];
        if (!r->cd_big) continue;
        const ds_subscription_t *sub = &s->subs[i];
        long reset_at;
        if (sub->win_5h_s > 0) {
            reset_at = sub->reset_5h;
        } else if (sub->week_s > 0) {
            reset_at = sub->reset_week;
        } else {
            continue; /* 余额型无倒计时 */
        }
        char buf[32];
        long remain = reset_at > now ? reset_at - now : 0;
        snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld",
                 remain / 3600, (remain % 3600) / 60, remain % 60);
        lv_label_set_text(r->cd_big, buf);
    }
}

/* 自动刷新（间隔由 Web「订阅自动刷新」下发，默认 15 分钟）：
 * 到点拉一次数据；订阅数量变化（增删实例）重建页面，否则就地更新。 */
/* ---- 订阅后台刷新（防网络冻屏，2026-11-16）----
 * ds_refresh 是阻塞 HTTP（Bridge 不可达时每个请求等 5s connect 超时），
 * 曾直接在 LVGL timer/按钮回调里同步调用——网络一断 LVGL 线程持显示锁
 * 停 5~25s，屏幕冻住（真机实测心跳 10s 才走一拍）。改 worker 线程拉数 +
 * lv_async_call 回 LVGL 线程更新；页面已退出（代际变化）则只更新数据不动 UI。 */
static volatile int g_q_gen;   /* 页面代际：quota_on_exit 自增 */
static volatile bool g_q_fetch;

typedef struct {
    int gen;
    int before;
    int app_id;
    int new_ivs; /* 回拉的刷新间隔（0=未拉到）；变了就地改 timer 周期 */
} quota_fetch_t;

static void quota_fetch_done(void *arg)
{
    quota_fetch_t *d = arg;
    g_q_fetch = false;
    if (d->gen == g_q_gen) { /* 页面还活着才动 UI（防悬垂控件） */
        /* 间隔配置热更新（2026-11-17）：Web 改完不必重启设备——
         * 每次拉数据顺带回拉 device config，变了就地改周期，
         * 「重开订阅页生效」从此为真（页面开着等下一 tick 也自愈） */
        if (d->new_ivs >= 60 && d->new_ivs != ds_config_refresh_interval()) {
            ds_config_set_refresh_interval(d->new_ivs);
            if (g_q_auto_timer) {
                lv_timer_set_period(g_q_auto_timer, (uint32_t)d->new_ivs * 1000u);
            }
            UILOG("quota auto refresh: interval -> %ds", d->new_ivs);
        }
        if (ds_state()->sub_count != d->before) {
            app_manager_open(d->app_id); /* 订阅数量变化：重建（空态↔列表） */
        } else {
            quota_update_all();
            /* 加载态回位（重建分支的新 tile 天然是"刷新"） */
            const gadget_state_t *s = ds_state();
            for (int i = 0; i < s->sub_count && i < DS_SUB_MAX; i++) {
                if (g_q[i].refresh_lbl) {
                    lv_label_set_text(g_q[i].refresh_lbl, "刷新");
                }
            }
        }
    }
    free(d);
}

static void *quota_fetch_worker(void *arg)
{
    ds_refresh();
    /* 顺带回拉运行配置：只在 worker 线程做阻塞 HTTP（坑 #11），
     * 只取间隔；亮度/音量是开机语义，不在此应用 */
    ds_device_config_t dc;
    memset(&dc, 0, sizeof(dc));
    if (ds_device_config_fetch(&dc) == 0 && dc.sub_refresh_interval_s >= 60) {
        ((quota_fetch_t *)arg)->new_ivs = dc.sub_refresh_interval_s;
    }
#ifdef ESP_PLATFORM
    UILOG("quota fetch: done stack_free=%u",
          (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    lv_async_call(quota_fetch_done, arg);
    return NULL;
}

/* 返回 0=已发起 -1=忙/资源不足（本轮跳过） */
static int quota_fetch_start(int app_id)
{
    if (g_q_fetch) return -1; /* 上一轮还在路上：忽略重入 */
    g_q_fetch = true;
    quota_fetch_t *d = calloc(1, sizeof(*d)); /* new_ivs=0：未拉到 */
    if (!d) {
        g_q_fetch = false;
        return -1;
    }
    d->gen = g_q_gen;
    d->before = ds_state()->sub_count;
    d->app_id = app_id;
#ifdef ESP_PLATFORM
    /* 真机：投常驻静态栈语音任务（坑 #15 模式）。pthread 走不通——
     * 2026-11-17 实测 largest_int=12276（差 12 字节凑不齐 12KB 栈+TCB），
     * 16KB/12KB 两档全灭 → "quota fetch: worker start failed" 每 60s
     * 刷屏、订阅页永远显示 TF 旧缓存。busy（语音占用中）跳过本轮，
     * 下一个自动刷新 tick 重试。 */
    int rc = ui_bg_task_post(quota_fetch_worker, d);
    if (rc != 0) {
        UILOG("quota fetch: bg task busy/skip rc=%d", rc);
        g_q_fetch = false;
        free(d);
        return -1;
    }
#else
    if (ui_worker_start(quota_fetch_worker, d, 16 * 1024) != 0) {
        UILOG("quota fetch: worker start failed");
        g_q_fetch = false;
        free(d);
        return -1;
    }
#endif
    return 0;
}

static void quota_auto_refresh_cb(lv_timer_t *t)
{
    (void)t;
    quota_fetch_start(g_q_app_id);
}

static void quota_on_exit(void)
{
    g_q_gen++; /* 在途 worker 的回调据此丢弃 UI 操作 */
    /* 两个 timer 已通过 app_manager_timer_create 登记，切走 App 时由
     * app_manager 统一删除（ADR-012 P2）；这里只断引用，避免悬垂。 */
    g_q_cd_timer = NULL;
    g_q_auto_timer = NULL;
    memset(g_q_dots, 0, sizeof(g_q_dots));
}

static void quota_refresh_cb(lv_event_t *e)
{
    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);
    if (quota_fetch_start(app_id) == 0) {
        /* 加载态：成功发起才改文案（busy 时按钮保持可用外观） */
        const gadget_state_t *s = ds_state();
        for (int i = 0; i < s->sub_count && i < DS_SUB_MAX; i++) {
            if (g_q[i].refresh_lbl) {
                lv_label_set_text(g_q[i].refresh_lbl, "刷新中…");
            }
        }
    }
}

/* ---------------- 占位页（上滑返回） ---------------- */

typedef struct {
    int app_id;
    const char *title;
    const char *phase;
} placeholder_meta_t;

static const placeholder_meta_t g_placeholders[] = {
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
    /* 抓取与注册分离：refresh 路径用 prefetch 抓好的 csv，避免持锁跑 HTTP */
    char layout[192];
    int lay_n = 0;
    bool ok = s_remote_layout_ready &&
              ds_launcher_fetch(layout, sizeof(layout), &lay_n) == 0 && lay_n > 0;
    register_apps_layout(ok ? layout : NULL, ok ? lay_n : 0);
}

static void register_apps_layout(const char *layout_csv, int lay_n)
{
    /* 顺序 = 桌面分页顺序（一屏 4 个，横滑翻页）：
     * 页1 AI 三位 + 订阅｜页2 天气/番茄钟/文件/设置｜页3 音乐
     * （2026-11-16 删 ZCode/服务器监控两 App，ADR-010；加番茄钟） */
    const app_def_t defs[] = {
        /* 头像 App：symbol/color 是头像素材缺位时的磁贴回退（NULL symbol
         * 会被 LVGL 渲染成默认 "Text"——真机/模拟器均踩过） */
        { UI_APP_CHAT,     "chat",     "AstrBot", LV_SYMBOL_ENVELOPE,    0x7C5CFF, "A:simulator/lvgl-pc/assets/avatars/chat.png",    true,  ui_conversation_build, ui_conversation_on_exit, 0 },
        { UI_APP_XIAOZHI,  "xiaozhi",  "小智",    LV_SYMBOL_AUDIO,   0x2EC4B6, "A:simulator/lvgl-pc/assets/avatars/xiaozhi.png", true,  ui_conversation_build, ui_conversation_on_exit, 0 },
        { UI_APP_MUSE,     "muse",     "Muse",    LV_SYMBOL_VIDEO,   0xFF6FA5, "A:simulator/lvgl-pc/assets/avatars/muse.png",    true,  ui_conversation_build, ui_conversation_on_exit, 0 },
        { UI_APP_ZHIPU,    "sub",      "订阅",    LV_SYMBOL_CHARGE,   0x2EC4B6, "A:simulator/lvgl-pc/assets/icons/sub.png",       true,  quota_build,           quota_on_exit, 0 },
        { UI_APP_WEATHER,  "weather",  "天气",    LV_SYMBOL_TINT,     0x4F8CFF, "A:simulator/lvgl-pc/assets/icons/weather.png",   true,  ui_weather_build,      ui_weather_on_exit, 0 },
        { UI_APP_POMODORO, "pomodoro", "番茄钟",  LV_SYMBOL_PLAY,     0xD94B3F, "A:simulator/lvgl-pc/assets/icons/pomodoro.png",  true,  ui_pomodoro_build,     ui_pomodoro_on_exit,   APP_FLAG_BG_TIMER | APP_FLAG_STATUSBAR },
        { UI_APP_FILES,    "files",    "文件",    LV_SYMBOL_DIRECTORY, 0xE0A030, "A:simulator/lvgl-pc/assets/icons/files.png",    true,  ui_files_build,        ui_files_on_exit, 0 },
        { UI_APP_SETTINGS, "settings", "设置",    LV_SYMBOL_SETTINGS, 0x5C6B8A, "A:simulator/lvgl-pc/assets/icons/settings.png",  true,  ui_settings_build,     ui_settings_on_exit, 0 },
        { UI_APP_MUSIC,    "music",    "音乐",    LV_SYMBOL_AUDIO,    0xFF4D6D, "A:simulator/lvgl-pc/assets/icons/music.png",     false, placeholder_build,     NULL, 0 },
    };
    const int def_n = (int)(sizeof(defs) / sizeof(defs[0]));

    /* Web 后台主页排布（/api/v1/launcher）：order **只决定顺序**——固件
     * 内置的 App 全部始终注册，远端没提到的追加到末尾。存在性归固件管，
     * 远端配置不负责显示/隐藏（2026-11-16 真机反馈：旧排布不含新 App
     * `files`，设备开机全量出现、联网拉到排布重建后却被藏掉）。
     * 可重复调用（清表重注册）：联网后 refresh 用预取的 csv 重跑本函数
     * + ui_launcher_rebuild。 */
    app_manager_clear();
    app_def_t ordered[16];
    int visible = 0;
    if (layout_csv && lay_n > 0) {
        bool used[16] = {false};
        int matched = 0;
        char tmp[192];
        snprintf(tmp, sizeof(tmp), "%s", layout_csv);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, ",", &save); tok && visible < 16;
             tok = strtok_r(NULL, ",", &save)) {
            for (int i = 0; i < def_n; i++) {
                if (used[i] || !defs[i].key || strcmp(defs[i].key, tok) != 0) continue;
                ordered[visible++] = defs[i];
                used[i] = true;
                matched++;
                break;
            }
        }
        /* 远端排布里没提到的内置 App：追加末尾（不是隐藏）；若 key 体系
         * 完全不兼容（一个都没匹配），追加结果即出厂全量，桌面不会空。 */
        for (int i = 0; i < def_n; i++) {
            if (!used[i]) ordered[visible++] = defs[i];
        }
        fprintf(stderr, "[apps] web layout: %d ordered, %d appended\n",
                matched, visible - matched);
    } else {
        for (int i = 0; i < def_n; i++) ordered[visible++] = defs[i];
    }

    for (int i = 0; i < visible; i++) {
        if (!app_manager_register(&ordered[i])) {
            fprintf(stderr, "[apps] app_manager_register failed at %d\n", i);
        }
    }
}
