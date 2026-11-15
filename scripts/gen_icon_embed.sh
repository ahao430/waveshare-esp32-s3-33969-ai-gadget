#!/bin/sh
# 桌面图标/头像 → 固件嵌入源（icon_embed.c/.h）——构建期预解码为
# LVGL 原生 RGB565（108×108，与磁贴显示尺寸 1:1）。
# 为什么不在设备上解码 PNG（esp_lv_decoder）：真机两组对照实测——
# 运行时解码 + 缩放 + alpha 混合，在滑动翻页/转场的全屏重绘时会
# 堵死显示管线（卡死）；预解码零运行时开销。
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
W = H = 108  # 与磁贴显示尺寸一致（1:1，运行时零缩放）

names = []
for d in src_dirs:
    for f in sorted(os.listdir(d)):
        if f.endswith(".png"):
            names.append((os.path.join(d, f), f[:-4]))

with open(os.path.join(out_dir, "icon_embed.c"), "w") as c:
    c.write("/* icon_embed — 自动生成，勿手改（scripts/gen_icon_embed.sh）。\n"
            " * PNG 构建期预解码为 RGB565（108x108，不透明素材）原生数组：\n"
            " * 运行时零解码、零缩放、零 alpha 混合（真机管线教训：\n"
            " * 9 张 ARGB+缩放图标在滑动/转场全屏重绘时堵死显示管线）。\n"
            " * 素材有透明通道时需改回 ARGB8888（脚本内会告警）。 */\n"
            "#include <string.h>\n"
            '#include "lvgl.h"\n'
            '#include "icon_embed.h"\n\n')
    for path, name in names:
        im = Image.open(path).convert("RGBA").resize((W, H), Image.LANCZOS)
        if im.split()[3].getextrema()[0] < 255:
            print(f"WARNING: {path} has transparency; RGB565 drops alpha")
        rgb = im.convert("RGB")
        px = rgb.load()
        c.write(f"static const uint8_t icon_{name}_rgb565[{W*H*2}] = {{\n")
        for y in range(H):
            row = []
            for x in range(W):
                r, g, b = px[x, y]
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                row.append(f"0x{v & 0xFF:02X},0x{v >> 8:02X}")
            c.write("    " + ",".join(row) + ",\n")
        c.write("};\n")
        c.write(f"static const lv_image_dsc_t icon_{name}_dsc = {{\n"
                f"    .header = {{ .cf = LV_COLOR_FORMAT_RGB565, .w = {W}, .h = {H},"
                f" .stride = {W*2} }},\n"
                f"    .data_size = sizeof(icon_{name}_rgb565),\n"
                f"    .data = icon_{name}_rgb565,\n"
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

print(f"生成完成: {len(names)} 个图标（RGB565 {W}x{H}，约 {len(names)*W*H*2//1024}KB flash）")
PYEOF
