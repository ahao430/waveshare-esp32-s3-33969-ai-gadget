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

/* 番茄钟大号倒计时（84px，仅 ASCII 空格+数字+冒号；Montserrat Medium，
 * 用 lv_font_conv 单独生成，见 scripts/gen_cn_font.sh 末尾。84px 保证
 * 四位数字 + 冒号在 340px 圆环内不压线） */
LV_FONT_DECLARE(font_digits_84);

/* 64px 档（番茄钟 v3：250px 渐变环内与吉祥物同处，四位数字收窄） */
LV_FONT_DECLARE(font_digits_64);

/* 56px 档（环缩至 220px 后，给吉祥物+数字让内径空间） */
LV_FONT_DECLARE(font_digits_56);

/* 番茄钟界面加粗字体（Noto Sans SC wght=700，只含该页字符串；见
 * scripts/gen_pomo_font.sh。用户反馈"文字要大、亮、粗"）。16px 档在
 * 2026-11-16 布局重做（统计改 20px）后不再使用，已从固件移除省 flash。 */
LV_FONT_DECLARE(font_pomo_20);

/* 注意：不要为单页再生成独立字库（曾做 font_sub_12/14/16 订阅页加粗
 * 全集三档，+1.67MB rodata 直接把 PSRAM 堆池从 4416K 挤到 ~3MB，TTS
 * 768KB 大块分配失败、三个聊天语音全哑——2026-11-17 实证已回退）。
 * rodata 与 PSRAM 堆共享 8MB 映射窗口（SPIRAM_RODATA=y，坑 #14 组合），
 * 加字库/大素材前先算池账。 */

#endif /* UI_FONT_CN_H */
