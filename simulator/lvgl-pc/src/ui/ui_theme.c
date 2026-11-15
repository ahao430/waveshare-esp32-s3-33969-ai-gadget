#include "ui_theme.h"

#include <time.h>

#ifdef ESP_PLATFORM
/* 显示锁实现（声明见 ui.h）：音频 worker/播放任务触碰 LVGL 前持有。
 * 必须用 esp_lv_adapter_lock(-1)：本版 BSP 的 bsp_display_lock(0) 实际是
 * 「0 tick 非阻塞 try-lock」（头文件写无限等待，与实现不符）——失败被忽略
 * 就变成并发改 LVGL，命中断言 "Invalidate area is not allowed during
 * rendering"，而 LV_ASSERT_HANDLER 默认 while(1) → 静默死机。
 * 递归互斥量，LVGL 线程内重入安全。 */
#include "esp_lv_adapter.h"
void ui_lock(void)   { (void)esp_lv_adapter_lock(-1); }
void ui_unlock(void) { esp_lv_adapter_unlock(); }
#endif

/* ---- 主题（用户 2026-11-14：主题色可换；0 号 = 定稿图基准） ---- */
const ui_theme_def_t k_ui_themes[UI_THEME_COUNT] = {
    { "星云紫", 0x7C5CFF, 0x4F8CFF, { 0x604CC8, 0x2482AA }, 0x2A3862 },
    { "极光青", 0x2EC4B6, 0x4F8CFF, { 0x1D7F8C, 0x2B5FA8 }, 0x1B3A46 },
    { "落日橙", 0xFF8A3D, 0xFF5D7A, { 0xA85A2E, 0x8C3A50 }, 0x3A2A30 },
    { "蔷薇粉", 0xFF6FA5, 0xB28DFF, { 0x9C4A78, 0x5A4A9C }, 0x33264A },
};

/* 零初始化：ui_init 会先 ui_theme_select(存档主题) 再建 UI */
lv_color_t g_ui_accent;
lv_color_t g_ui_accent2;

static int g_theme_idx;

void ui_theme_select(int idx)
{
    if (idx < 0 || idx >= UI_THEME_COUNT) idx = 0;
    g_theme_idx = idx;
    g_ui_accent  = lv_color_hex(k_ui_themes[idx].accent);
    g_ui_accent2 = lv_color_hex(k_ui_themes[idx].accent2);
}

int ui_theme_current(void) { return g_theme_idx; }

void ui_style_card(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, UI_CARD, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 24, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

lv_color_t ui_pct_color(int used_pct)
{
    if (used_pct >= 85) return UI_ERR;
    if (used_pct >= 60) return UI_WARN;
    return UI_ACCENT;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

void ui_fmt_hhmm(long unix_ts, char *buf, int buflen)
{
    time_t t = (time_t)unix_ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, (size_t)buflen, "%H:%M", &tmv);
}

void ui_fmt_md_hhmm(long unix_ts, char *buf, int buflen)
{
    time_t t = (time_t)unix_ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, (size_t)buflen, "%m-%d %H:%M", &tmv);
}
