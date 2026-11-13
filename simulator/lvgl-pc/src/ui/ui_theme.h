/* ui_theme — 深色 OLED 友好主题（PLAN §8.1）：深蓝黑底、紫蓝强调、高对比文字。 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "fonts/ui_font_cn.h"
#include "lvgl.h"

#define UI_BG        lv_color_hex(0x0B1020) /* 深蓝黑背景 */
#define UI_CARD      lv_color_hex(0x161D31) /* 玻璃拟态卡片底 */
#define UI_CARD_HI   lv_color_hex(0x1F2A47) /* 卡片高亮（按下/悬停） */
#define UI_LINE      lv_color_hex(0x212A44) /* 描边/分隔线（与 Web --line 一致） */
#define UI_ACCENT    lv_color_hex(0x7C5CFF) /* 紫蓝主强调 */
#define UI_ACCENT2   lv_color_hex(0x4F8CFF) /* 渐变副色 */
#define UI_OK        lv_color_hex(0x3DDC84)
#define UI_WARN      lv_color_hex(0xFFB13D)
#define UI_ERR       lv_color_hex(0xFF5D5D)
#define UI_TEXT      lv_color_hex(0xEAF0FF)
#define UI_TEXT_DIM  lv_color_hex(0x8A93AD)

/* 通用卡片样式：圆角、卡片底色、内边距 */
void ui_style_card(lv_obj_t *obj);

/* 按百分比选状态色（额度展示用） */
lv_color_t ui_pct_color(int used_pct);

/* 创建带样式的标题文字 */
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   lv_color_t color);

/* 时间格式化（本地时区）：unix 秒 → "HH:MM" / "MM-DD HH:MM" */
void ui_fmt_hhmm(long unix_ts, char *buf, int buflen);
void ui_fmt_md_hhmm(long unix_ts, char *buf, int buflen);

#endif /* UI_THEME_H */
