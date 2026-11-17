/* ui_theme — 深色 OLED 友好主题（PLAN §8.1）：深蓝黑底、多主题强调色、高对比文字。
 * 主题：UI_ACCENT/UI_ACCENT2 为全局变量宏（引用点零改动），
 * ui_theme_select 切换后需重建 UI（ui_apply_theme_rebuild）。 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "fonts/ui_font_cn.h"
#include "lvgl.h"

#define UI_BG        lv_color_hex(0x000000) /* AMOLED 纯黑背景 */
#define UI_CARD      lv_color_hex(0x161D31) /* 玻璃拟态卡片底 */
#define UI_CARD_HI   lv_color_hex(0x1F2A47) /* 卡片高亮（按下/悬停） */
#define UI_LINE      lv_color_hex(0x212A44) /* 描边/分隔线（与 Web --line 一致） */

/* 可变强调色（主题项）：宏引用保持不变，切换主题 = 改变量 + 重建 UI */
extern lv_color_t g_ui_accent;
extern lv_color_t g_ui_accent2;
#define UI_ACCENT  g_ui_accent
#define UI_ACCENT2 g_ui_accent2

#define UI_OK        lv_color_hex(0x3DDC84)
#define UI_WARN      lv_color_hex(0xFFB13D)
#define UI_ERR       lv_color_hex(0xFF5D5D)
#define UI_TEXT      lv_color_hex(0xEAF0FF)
#define UI_TEXT_DIM  lv_color_hex(0x8A93AD)

/* ---- 主题预设（用户 2026-11-14 需求：主题色可换） ---- */
typedef struct {
    const char *name;    /* 中文名（设置页显示） */
    uint32_t accent;     /* 主强调 */
    uint32_t accent2;    /* 副色 */
    uint32_t wave[2];    /* 桌面波浪两层色 */
    uint32_t glow;       /* 桌面顶部光晕目标色 */
} ui_theme_def_t;

#define UI_THEME_COUNT 4
extern const ui_theme_def_t k_ui_themes[UI_THEME_COUNT];

/* 切换主题（越界钳到 0；只改变量，UI 重建由调用方负责） */
void ui_theme_select(int idx);
int ui_theme_current(void);

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
