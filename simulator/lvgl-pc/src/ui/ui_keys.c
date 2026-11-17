/* ui_keys — 实现，接口见 ui_keys.h。 */
#include "ui_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "ui.h"

/* KEY3 短按/长按的 App 自定义处理（App 可注册；聊天=连续对话/PTT） */
static void (*g_key3_cb)(void);
static void (*g_key3_long_cb)(void);
/* 长按释放（真机按住 KEY3 说话、松开提交的收口；模拟器无释放事件源） */
static void (*g_key3_long_rel_cb)(void);

/* Web 可下发映射（默认=出厂行为，POWER_MANAGEMENT §13.4） */
static struct {
    char pwr_short[8];
    char boot_short[8];
    char key3_short[8];
    char key3_long[8];
} g_cfg = { UI_KEY_ACT_HOME, UI_KEY_ACT_BACK, UI_KEY_ACT_APP, UI_KEY_ACT_PTT };

void ui_key_set_key3_handler(void (*cb)(void))
{
    g_key3_cb = cb;
}

void ui_key_set_key3_long_handler(void (*cb)(void))
{
    g_key3_long_cb = cb;
}

void ui_key_set_key3_long_release_handler(void (*cb)(void))
{
    g_key3_long_rel_cb = cb;
}

/* 动作执行：home/back/none 之外由调用方处理（app → 回调） */
static void action_exec(const char *act, void (*app_cb)(void))
{
    if (strcmp(act, UI_KEY_ACT_HOME) == 0) {
        ui_show_launcher();
    } else if (strcmp(act, UI_KEY_ACT_BACK) == 0) {
        app_manager_back();
    } else if (strcmp(act, UI_KEY_ACT_APP) == 0) {
        if (app_cb) app_cb();
    } /* none：无操作 */
}

void ui_key_dispatch(ui_key_t key)
{
    switch (key) {
    case UI_KEY_PWR:
        /* 2026-11-17 用户拍板：PWR 短按=息屏/点亮切换，固件固定行为。
         * 屏保在真机 main.c（saver_enter/saver_exit），模拟器无此设施，
         * 无操作；g_cfg.pwr_short 仅为配置链兼容保留，不再消费。 */
        break;
    case UI_KEY_BOOT:
        action_exec(g_cfg.boot_short, NULL);
        break;
    case UI_KEY_KEY3:
        action_exec(g_cfg.key3_short, g_key3_cb);
        break;
    }
}

void ui_key_dispatch_long(ui_key_t key)
{
    if (key != UI_KEY_KEY3) return; /* PWR/BOOT 长按在固件侧（不可配置） */
    if (strcmp(g_cfg.key3_long, UI_KEY_ACT_PTT) == 0) {
        if (g_key3_long_cb) g_key3_long_cb();
    } /* none：无操作 */
}

void ui_key_dispatch_long_release(ui_key_t key)
{
    if (key != UI_KEY_KEY3) return;
    if (strcmp(g_cfg.key3_long, UI_KEY_ACT_PTT) == 0) {
        if (g_key3_long_rel_cb) g_key3_long_rel_cb();
    }
}

/* 只接受已知动作值，未知/NULL 保留现值 */
static void cfg_set(char *slot, const char *v)
{
    if (!v || !v[0]) return;
    if (strcmp(v, UI_KEY_ACT_HOME) != 0 && strcmp(v, UI_KEY_ACT_BACK) != 0 &&
        strcmp(v, UI_KEY_ACT_NONE) != 0 && strcmp(v, UI_KEY_ACT_APP) != 0 &&
        strcmp(v, UI_KEY_ACT_PTT) != 0) {
        return;
    }
    snprintf(slot, 8, "%s", v);
}

void ui_keys_apply_config(const char *pwr_short, const char *boot_short,
                          const char *key3_short, const char *key3_long)
{
    cfg_set(g_cfg.pwr_short, pwr_short);
    cfg_set(g_cfg.boot_short, boot_short);
    cfg_set(g_cfg.key3_short, key3_short);
    cfg_set(g_cfg.key3_long, key3_long);
    fprintf(stderr, "[keys] cfg pwr=%s boot=%s key3s=%s key3l=%s\n",
            g_cfg.pwr_short, g_cfg.boot_short, g_cfg.key3_short, g_cfg.key3_long);
}

/* ---- GADGET_DEMO_KEYS 自动化 ---- */

typedef struct {
    int ms;
    ui_key_t key;
    int long_press; /* 1=长按事件 */
} key_plan_t;

static key_plan_t g_plans[16];
static int g_plan_n;

static void key_plan_fire(lv_timer_t *t)
{
    key_plan_t *p = lv_timer_get_user_data(t);
    if (p->long_press) ui_key_dispatch_long(p->key);
    else ui_key_dispatch(p->key);
    fprintf(stderr, "[keys] fired %d%s\n", (int)p->key, p->long_press ? "(long)" : "");
    lv_timer_del(t);
}

static void key_plan_start(lv_timer_t *t)
{
    lv_timer_del(t);
    for (int i = 0; i < g_plan_n; i++) {
        lv_timer_t *kt = lv_timer_create(key_plan_fire, g_plans[i].ms, &g_plans[i]);
        (void)kt;
    }
}

void ui_keys_demo_init(void)
{
    const char *spec = getenv("GADGET_DEMO_KEYS");
    if (!spec || !spec[0]) return;
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && g_plan_n < 16;
         tok = strtok_r(NULL, ",", &save)) {
        int ms = 2000;
        char name[16] = "";
        if (sscanf(tok, "%15[^@]@%d", name, &ms) < 1) continue;
        int long_press = 0;
        ui_key_t k;
        if (strcmp(name, "pw") == 0) k = UI_KEY_PWR;
        else if (strcmp(name, "boot") == 0) k = UI_KEY_BOOT;
        else if (strcmp(name, "key3") == 0) k = UI_KEY_KEY3;
        else if (strcmp(name, "key3long") == 0) { k = UI_KEY_KEY3; long_press = 1; }
        else continue;
        g_plans[g_plan_n].ms = ms;
        g_plans[g_plan_n].key = k;
        g_plans[g_plan_n].long_press = long_press;
        g_plan_n++;
    }
    if (g_plan_n > 0) {
        lv_timer_t *t = lv_timer_create(key_plan_start, 10, NULL);
        (void)t;
    }
}
