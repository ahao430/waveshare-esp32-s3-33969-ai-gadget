/* 模拟器专用 LVGL 配置（LVGL v9.2）。
 * 只覆盖与默认值不同的项；其余由 lv_conf_internal.h 提供默认。
 * 固件侧（ESP-IDF）使用 Kconfig 生成配置，不共用本文件。
 * LV_USE_SDL 是模拟器与固件唯一的结构性差异（显示/输入驱动层）。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 480×480 AMOLED 用 RGB565，与真机一致 */
#define LV_COLOR_DEPTH 16

/* PC 内存充裕：1MB 内置池（256KB 时语音页对象+GIF 解码缓冲会耗尽池，
 * lv_obj_create 返回 NULL → 后续 style 操作 SIGSEGV——崩溃报告定位）。
 * 固件侧按 PLAN §14 预算另配。 */
/* 4MB：480×480 PNG 背景解码需 ~921KB RGBA + 绘制缓存（1MB 池下
 * 自定义背景解码失败表现为静默空白——目视+像素验收定位） */
#define LV_MEM_SIZE (4 * 1024 * 1024)

/* SDL 显示/输入驱动（lv_sdl_window / lv_sdl_mouse） */
#define LV_USE_SDL 1

/* GIF 解码（Muse 吉祥物动画；固件侧按需裁剪，PLAN §8.5） */
#define LV_USE_GIF 1

/* 二维码组件（设置页：配网入口 / 局域网管理地址；PLAN §9.3） */
#define LV_USE_QRCODE 1

/* PNG 解码（小智 Q 版形象；固件侧转 RGB565 C 数组） */
#define LV_USE_LODEPNG 1

/* stdio 文件系统（GIF 等资源按相对 CWD 路径加载，盘符 'A'） */
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'A'
#define LV_FS_STDIO_PATH 

/* 字体：模拟器 UI 使用的全部字号（真机按需裁剪） */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

/* TODO: 中文 UI 需要 CJK 点阵字体，后续用 lv_font_conv 从思源黑体生成子集，
 * 模拟器与固件共用同一字体文件（ADR-001）。 */

/* 离屏截图（main.c GADGET_SNAP 钩子）：headless UI 验收用 */
#define LV_USE_SNAPSHOT 1

#endif /* LV_CONF_H */
