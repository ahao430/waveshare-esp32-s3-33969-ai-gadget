/* ui_keys — 实现，接口见 ui_keys.h。 */
#include "ui_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "ui.h"

/* KEY3 短按的 App 自定义处理（App 可注册；v1：对话 App=连续对话） */
static void (*g_key3_cb)(void);

void ui_key_set_key3_handler(void (*cb)(void))
{
    g_key3_cb = cb;
}

void ui_key_dispatch(ui_key_t key)
{
    switch (key) {
    case UI_KEY_PWR:
        ui_show_launcher(); /* 回首页 */
        break;
    case UI_KEY_BOOT:
        app_manager_back(); /* 返回 */
        break;
    case UI_KEY_KEY3:
        if (g_key3_cb) g_key3_cb();
        break;
    }
}

/* ---- GADGET_DEMO_KEYS 自动化 ---- */

typedef struct {
    int ms;
    ui_key_t key;
} key_plan_t;

static key_plan_t g_plans[16];
static int g_plan_n;

static void key_plan_fire(lv_timer_t *t)
{
    key_plan_t *p = lv_timer_get_user_data(t);
    ui_key_dispatch(p->key);
    fprintf(stderr, "[keys] fired %d\n", (int)p->key);
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
        ui_key_t k;
        if (strcmp(name, "pw") == 0) k = UI_KEY_PWR;
        else if (strcmp(name, "boot") == 0) k = UI_KEY_BOOT;
        else if (strcmp(name, "key3") == 0) k = UI_KEY_KEY3;
        else continue;
        g_plans[g_plan_n].ms = ms;
        g_plans[g_plan_n].key = k;
        g_plan_n++;
    }
    if (g_plan_n > 0) {
        lv_timer_t *t = lv_timer_create(key_plan_start, 10, NULL);
        (void)t;
    }
}
