/* 模拟器专用 LVGL 配置（LVGL v9.2）。
 * 只覆盖与默认值不同的项；其余由 lv_conf_internal.h 提供默认。
 * 固件侧（ESP-IDF）使用 Kconfig 生成配置，不共用本文件。
 * LV_USE_SDL 是模拟器与固件唯一的结构性差异（显示/输入驱动层）。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 480×480 AMOLED 用 RGB565，与真机一致 */
#define LV_COLOR_DEPTH 16

/* PC 内存充裕；固件侧会显著调小并评估 PSRAM 策略（PLAN §14） */
#define LV_MEM_SIZE (256 * 1024)

/* SDL 显示/输入驱动（lv_sdl_window / lv_sdl_mouse） */
#define LV_USE_SDL 1

/* GIF 解码（Muse 吉祥物动画；固件侧按需裁剪，PLAN §8.5） */
#define LV_USE_GIF 1

/* 字体：模拟器 UI 使用的全部字号（真机按需裁剪） */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

/* TODO: 中文 UI 需要 CJK 点阵字体，后续用 lv_font_conv 从思源黑体生成子集，
 * 模拟器与固件共用同一字体文件（ADR-001）。 */

#endif /* LV_CONF_H */
