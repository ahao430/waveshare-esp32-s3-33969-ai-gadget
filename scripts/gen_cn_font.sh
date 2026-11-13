#!/usr/bin/env bash
# 生成中文字体子集（Noto Sans SC，OFL）→ simulator src/ui/fonts/*.c
#
# 依赖：node/npx（lv_font_conv）、python3（字符集提取）。
# 字体 TTF 默认缓存在 /tmp/cnfont/NotoSansSC.ttf（17MB，variable wght），
# 不存在时自动从 google/fonts 仓库下载。
#
# 用法：改了 UI 中文文案后运行 scripts/gen_cn_font.sh，然后重编模拟器。
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_TTF="${FONT_TTF:-/tmp/cnfont/NotoSansSC.ttf}"
OUT_DIR="simulator/lvgl-pc/src/ui/fonts"
SIZES="12 14 16 20"

if [ ! -f "$FONT_TTF" ]; then
  echo "== 下载 Noto Sans SC（google/fonts，OFL）"
  mkdir -p "$(dirname "$FONT_TTF")"
  curl -fL --retry 3 -o "$FONT_TTF" \
    "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
fi

echo "== 提取 UI 源码中的字符集"
SYMBOLS=$(python3 - <<'PYEOF'
import glob, sys

chars = set()
for path in glob.glob("simulator/lvgl-pc/src/ui/*.c") + glob.glob("simulator/lvgl-pc/src/data/*.c"):
    with open(path, encoding="utf-8", errors="ignore") as f:
        for ch in f.read():
            if ord(ch) > 0x7E:  # 非 ASCII（含 CJK、全角标点、间隔号等）
                chars.add(ch)

# 保险冗余：数字/常用标点已在 ASCII range；全角括号与间隔号常见于格式化字符串
chars |= set("（）【】·％、，。：；！？")
sys.stdout.write("".join(sorted(chars)))
PYEOF
)
echo "   共 ${#SYMBOLS} 个非 ASCII 字符"

mkdir -p "$OUT_DIR"
for sz in $SIZES; do
  echo "== 生成 ${sz}px"
  npx --yes lv_font_conv \
    --font "$FONT_TTF" \
    --size "$sz" \
    --bpp 4 \
    --no-compress \
    --no-kerning \
    --format lvgl \
    --range "0x20-0x7F" \
    --symbols "$SYMBOLS" \
    --lv-include "lvgl.h" \
    --lv-font-name "lv_font_cn_${sz}" \
    -o "$OUT_DIR/font_cn_${sz}.c"
done

echo "完成：$OUT_DIR/font_cn_{12,14,16,20}.c（请重编模拟器）"
