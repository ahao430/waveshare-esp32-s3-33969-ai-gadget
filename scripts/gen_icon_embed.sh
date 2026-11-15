#!/bin/sh
# 桌面图标/头像 → 固件嵌入源（icon_embed.c/.h）——构建期预解码为
# LVGL 原生 ARGB8888 数组（lv_image_dsc_t 直接可画）。
# 为什么不用运行时 PNG 解码（esp_lv_decoder）：真机实测缓存 256KB
# 放不下 9 张 88×88 解码结果（约 279KB）→ 逐出反复重解码，首帧卡死
# +滑动堵管线（2026-11-15 真机验证）；预解码零运行时开销。
# 素材改动后重跑本脚本 + 重建固件（模拟器仍走文件路径 PNG）。
set -e
cd "$(dirname "$0")/.."
OUT_DIR=simulator/lvgl-pc/src/ui/icons
mkdir -p "$OUT_DIR"

python3 - "$OUT_DIR" <<'PYEOF'
import sys, os
from PIL import Image

out_dir = sys.argv[1]
src_dirs = ["simulator/lvgl-pc/assets/icons",
            "simulator/lvgl-pc/assets/avatars"]
W = H = 88  # 统一尺寸（与磁贴 ICON 缩放配合）

names = []
for d in src_dirs:
    for f in sorted(os.listdir(d)):
        if f.endswith(".png"):
            names.append((os.path.join(d, f), f[:-4]))

with open(os.path.join(out_dir, "icon_embed.c"), "w") as c:
    c.write("/* icon_embed — 自动生成，勿手改（scripts/gen_icon_embed.sh）。\n"
            " * PNG 构建期预解码为 ARGB8888（88×88）原生数组，lv_image_dsc_t\n"
            " * 直接可画（无运行时解码/缓存依赖）。模拟器走文件路径不经此文件。 */\n"
            "#include <string.h>\n"
            '#include "lvgl.h"\n'
            '#include "icon_embed.h"\n\n')
    for path, name in names:
        im = Image.open(path).convert("RGBA").resize((W, H))
        px = im.load()
        c.write(f"static const uint8_t icon_{name}_argb[{W*H*4}] = {{\n")
        for y in range(H):
            row = []
            for x in range(W):
                r, g, b, a = px[x, y]
                row.append(f"0x{b:02X},0x{g:02X},0x{r:02X},0x{a:02X}")
            c.write("    " + ",".join(row) + ",\n")
        c.write("};\n")
        c.write(f"static const lv_image_dsc_t icon_{name}_dsc = {{\n"
                f"    .header = {{ .cf = LV_COLOR_FORMAT_ARGB8888, .w = {W}, .h = {H} }},\n"
                f"    .data_size = sizeof(icon_{name}_argb),\n"
                f"    .data = icon_{name}_argb,\n"
                "};\n\n")
    c.write("const lv_image_dsc_t *ui_icon_find(const char *key)\n{\n")
    for _, name in names:
        c.write(f'    if (strcmp(key, "{name}") == 0) return &icon_{name}_dsc;\n')
    c.write("    return NULL;\n}\n")

with open(os.path.join(out_dir, "icon_embed.h"), "w") as h:
    h.write("/* icon_embed — 自动生成，勿手改。按文件名（无扩展名）查嵌入图标。 */\n"
            "#ifndef ICON_EMBED_H\n#define ICON_EMBED_H\n\n"
            '#include "lvgl.h"\n\n'
            "/* key=图标文件名（如 \"sub\"）；未嵌入返回 NULL（调用方回退占位）。 */\n"
            "const lv_image_dsc_t *ui_icon_find(const char *key);\n\n"
            "#endif /* ICON_EMBED_H */\n")

print(f"生成完成: {len(names)} 个图标（ARGB8888 {W}x{H}，约 {len(names)*W*H*4//1024}KB flash）")
PYEOF
