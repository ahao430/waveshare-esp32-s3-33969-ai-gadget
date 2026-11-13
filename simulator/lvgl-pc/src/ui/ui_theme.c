#include "ui_theme.h"

#include <time.h>

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
