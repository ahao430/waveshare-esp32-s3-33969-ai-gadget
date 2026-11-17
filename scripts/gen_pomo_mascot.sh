#!/bin/sh
# 番茄钟吉祥物 → 固件嵌入源（pomo_mascot.c/.h）——构建期预解码为
# LVGL 原生 RGB565（76×76 不透明，黑底与页面深色底自然融合）。
# 素材：assets/ui/番茄专注休息表情包.png（2×2 四宫格：专注/休息/长休/完成），
# 每格裁掉底部标签胶囊（上 75% 区域），再按亮度阈值去黑边、取正方形。
# 素材改动后重跑本脚本 + 重建（模拟器同走嵌入数组，双平台一致）。
set -e
cd "$(dirname "$0")/.."
OUT_DIR=simulator/lvgl-pc/src/ui/icons
SRC="assets/ui/番茄专注休息表情包.png"
mkdir -p "$OUT_DIR"

python3 - "$OUT_DIR" "$SRC" <<'PYEOF'
import sys, os
from PIL import Image

out_dir, src = sys.argv[1], sys.argv[2]
img = Image.open(src).convert("RGB")
W, H = img.size
qw, qh = W // 2, H // 2
SIZE = 64

# 四宫格 → 状态：左上=专注 右上=休息 左下=长休 右下=完成
cells = [("focus", 0, 0), ("rest", 1, 0), ("long", 0, 1), ("done", 1, 1)]

def cell_image(cx, cy):
    # 四边内缩 5%：相邻格的光晕会跨过格线，收进可避免溢色残留；
    # 底部只取 72%：下半是文字标签胶囊，不进吉祥物
    x0 = cx * qw + int(qw * 0.05)
    x1 = (cx + 1) * qw - int(qw * 0.05)
    y0 = cy * qh + int(qh * 0.03)
    y1 = cy * qh + int(qh * 0.72)
    c = img.crop((x0, y0, x1, y1))
    # 去黑边：找非暗像素的包围盒（含彩色光晕），留 6px 呼吸边
    px = c.load()
    x0, y0, x1, y1 = c.width, c.height, 0, 0
    for y in range(c.height):
        for x in range(c.width):
            r, g, b = px[x, y]
            if r > 36 or g > 36 or b > 36:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 <= x0 or y1 <= y0:
        return c.resize((SIZE, SIZE), Image.LANCZOS)
    pad = 6
    x0 = max(0, x0 - pad); y0 = max(0, y0 - pad)
    x1 = min(c.width - 1, x1 + pad); y1 = min(c.height - 1, y1 + pad)
    # 取正方形（包围盒中心扩展）
    cw, ch = x1 - x0 + 1, y1 - y0 + 1
    side = max(cw, ch)
    cxm, cym = (x0 + x1) // 2, (y0 + y1) // 2
    half = side // 2
    sx0 = max(0, cxm - half); sy0 = max(0, cym - half)
    sx1 = min(c.width, sx0 + side); sy1 = min(c.height, sy0 + side)
    sx0 = max(0, sx1 - side); sy0 = max(0, sy1 - side)
    return c.crop((sx0, sy0, sx1, sy1)).resize((SIZE, SIZE), Image.LANCZOS)

def circle_mask(im):
    """圆形软蒙版：边缘光晕/邻格溢色渐隐到纯黑（吉祥物居中，环内
    展示本身就是圆形语境，边缘裁圆无违和且保证四边干净）。"""
    w, h = im.size
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    r_out = min(w, h) / 2.0 - 1
    r_in = r_out - 6
    px = im.load()
    for y in range(h):
        for x in range(w):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if d >= r_out:
                px[x, y] = (0, 0, 0)
            elif d > r_in:
                k = (d - r_in) / (r_out - r_in)
                r, g, b = px[x, y]
                px[x, y] = (int(r * (1 - k)), int(g * (1 - k)), int(b * (1 - k)))
    return im

def rgb565(im):
    out = bytearray()
    for (r, g, b) in im.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        # LVGL RGB565 按平台原生 uint16 读——小端（ESP32/Mac）低字节在前；
        # 首版写反（高字节在前）→ 通道错位，番茄渲染成彩虹噪点
        out.append(v & 0xFF)
        out.append((v >> 8) & 0xFF)
    return bytes(out)

names = []
for name, cx, cy in cells:
    im = circle_mask(cell_image(cx, cy))
    im.save(os.path.join(out_dir, "pomo_%s_preview.png" % name))
    with open(os.path.join(out_dir, "pomo_mascot_%s.bin" % name), "wb") as f:
        f.write(rgb565(im))
    names.append(name)

with open(os.path.join(out_dir, "pomo_mascot.c"), "w") as c:
    c.write("/* pomo_mascot — 自动生成，勿手改（scripts/gen_pomo_mascot.sh）。\n"
            " * 番茄钟吉祥物四态（专注/休息/长休/完成），RGB565 76×76 黑底。\n"
            " * 运行时零解码零缩放（同 icon_embed.c 管线教训）。 */\n"
            "#include \"pomo_mascot.h\"\n\n")
    for name in names:
        with open(os.path.join(out_dir, "pomo_mascot_%s.bin" % name), "rb") as f:
            data = f.read()
        c.write("static const uint8_t px_%s[] = {" % name)
        c.write(",".join(str(b) for b in data))
        c.write("};\n")
        c.write("const lv_image_dsc_t pomo_mascot_%s = {\n" % name)
        c.write("    .header = { .w = %d, .h = %d, .cf = LV_COLOR_FORMAT_RGB565 },\n" % (SIZE, SIZE))
        c.write("    .data_size = sizeof(px_%s), .data = px_%s,\n" % (name, name))
        c.write("};\n\n")

with open(os.path.join(out_dir, "pomo_mascot.h"), "w") as h:
    h.write("/* pomo_mascot — 自动生成（scripts/gen_pomo_mascot.sh） */\n"
            "#ifndef POMO_MASCOT_H\n#define POMO_MASCOT_H\n\n"
            '#include "lvgl.h"\n\n')
    for name in names:
        h.write("extern const lv_image_dsc_t pomo_mascot_%s;\n" % name)
    h.write("\n#endif /* POMO_MASCOT_H */\n")

for f in os.listdir(out_dir):
    if f.endswith("_preview.png"):
        print("preview:", os.path.join(out_dir, f))
print("OK: %d mascots -> %s" % (len(names), out_dir))
PYEOF
