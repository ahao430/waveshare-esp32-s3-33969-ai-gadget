#include "app_manager.h"

#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

/* 内存观测（ADR-011）：进/出 App 各打一条，实测各 App 的 RAM 成本。
 * 指标：总空闲 / 历史最低 / PSRAM 空闲 / 内部 RAM 最大连续块。 */
static void appmem_log(const char *evt, const app_def_t *def)
{
    ESP_LOGI("appmem", "%s %s heap=%u min=%u psram=%u largest_int=%u",
             evt, (def && def->key) ? def->key : "?",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
#else
#define appmem_log(evt, def) ((void)0)
#endif

#define APP_MAX 16

static app_def_t g_apps[APP_MAX];
static int g_count;

static lv_obj_t *g_home;
static lv_obj_t *g_current_screen;
static const app_def_t *g_current_def;

bool app_manager_register(const app_def_t *def)
{
    if (!def || g_count >= APP_MAX) return false;
    g_apps[g_count++] = *def;
    return true;
}

void app_manager_clear(void)
{
    /* 调用方保证当前不在 App 内（g_current_def 指向表内元素） */
    g_count = 0;
}

int app_manager_count(void) { return g_count; }

const app_def_t *app_manager_get(int idx)
{
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_apps[idx];
}

const app_def_t *app_manager_find(int app_id)
{
    /* 按 id 查（ui_app_id 是枚举值，不等于注册序号——曾用 get(app_id)
     * 取到隔壁 App 的 key：AstrBot(4) → 注册序 4 = ZCode） */
    for (int i = 0; i < g_count; i++) {
        if (g_apps[i].id == app_id) return &g_apps[i];
    }
    return NULL;
}

void app_manager_set_home(lv_obj_t *home) { g_home = home; }

static void exit_current_app_logic(void)
{
    /* 只执行 App 级清理；屏幕对象的销毁交给转场 auto_del，
     * 避免动画进行中删除被渲染的屏幕。 */
    if (g_current_def && g_current_def->on_exit) g_current_def->on_exit();
}

void app_manager_open(int app_id)
{
    const app_def_t *def = NULL;
    for (int i = 0; i < g_count; i++) {
        if (g_apps[i].id == app_id) { def = &g_apps[i]; break; }
    }
    if (!def || !g_home) return;

    bool in_app = (g_current_screen != NULL);
    exit_current_app_logic();
    appmem_log("open-before", def);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    if (def->build) def->build(scr, app_id);

    /* auto_del=true：转场结束后销毁被替换的屏幕（资源释放，PLAN §4.3） */
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, in_app);
    g_current_screen = scr;
    g_current_def = def;
    appmem_log("open-after", def);
}

void app_manager_back(void)
{
    if (!g_current_screen) return;
    appmem_log("back", g_current_def);
    exit_current_app_logic();
    g_current_screen = NULL;
    g_current_def = NULL;

    /* 转场回 Home，动画结束后自动删除 App 屏幕 */
    lv_screen_load_anim(g_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    app_manager_back();
}

void app_manager_back_on_click(lv_obj_t *button)
{
    lv_obj_add_event_cb(button, back_click_cb, LV_EVENT_CLICKED, NULL);
}

bool app_manager_in_app(void) { return g_current_screen != NULL; }

int app_manager_current_id(void)
{
    return g_current_def ? g_current_def->id : -1;
}
