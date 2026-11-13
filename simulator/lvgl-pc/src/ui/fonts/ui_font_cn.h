/* 中文字体（思源黑体系 → Noto Sans SC 子集，OFL 协议，可嵌入固件）。
 * 由 scripts/gen_cn_font.sh 生成：
 *   - 扫描 src/ui、src/data 源码中的全部 CJK/全角字符 + ASCII
 *   - lv_font_conv 生成 12/14/16/20 四个字号（4bpp、不压缩）
 * 字体文件提交进仓库，平时无需重新生成；改了 UI 文案后再跑脚本。
 */
#ifndef UI_FONT_CN_H
#define UI_FONT_CN_H

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_cn_12);
LV_FONT_DECLARE(lv_font_cn_14);
LV_FONT_DECLARE(lv_font_cn_16);
LV_FONT_DECLARE(lv_font_cn_20);

#endif /* UI_FONT_CN_H */
