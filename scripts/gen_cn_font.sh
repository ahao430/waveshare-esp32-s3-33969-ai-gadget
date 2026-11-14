#!/usr/bin/env bash
# 生成中文+符号字体子集（Noto Sans SC + FontAwesome5）→ simulator src/ui/fonts/*.c
#
# 背景：cn 字体与 LV_SYMBOL 混排（下拉箭头/图标+中文同 label）时因 cn 缺
# FontAwesome 区段字形渲染豆腐——双字体源合并：Noto 出 ASCII+CJK，
# lvgl 自带 FontAwesome5.woff 出 LV_SYMBOL_* 全集。
#
# 依赖：node/npx（lv_font_conv）、python3（字符集提取）。
# 字体 TTF 默认缓存在 /tmp/cnfont/NotoSansSC.ttf（17MB，variable wght），
# 不存在时自动从 google/fonts 仓库下载。
#
# 用法：改了 UI 中文文案后运行 scripts/gen_cn_font.sh，然后重编模拟器。
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_TTF="${FONT_TTF:-/tmp/cnfont/NotoSansSC.ttf}"
FA_WOFF="simulator/lvgl-pc/build/_deps/lvgl-src/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
OUT_DIR="simulator/lvgl-pc/src/ui/fonts"
SIZES="12 14 16 20"

if [ ! -f "$FONT_TTF" ]; then
  echo "== 下载 Noto Sans SC（google/fonts，OFL）"
  mkdir -p "$(dirname "$FONT_TTF")"
  curl -fL --retry 3 -o "$FONT_TTF" \
    "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
fi

if [ ! -f "$FA_WOFF" ]; then
  echo "错误：未找到 FontAwesome 字体（$FA_WOFF）——先完成模拟器首次构建" >&2
  exit 1
fi

echo "== 提取 UI 源码字符集 + LVGL symbol 集"
SYMBOLS=$(python3 - <<'PYEOF'
import glob, re, sys

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
FA_SYMBOLS=$(python3 - <<'PYEOF'
import re, sys
# lv_symbol_def.h 的 LV_SYMBOL_*（"\xEF.." C 转义字节串 = UTF-8 字符），
# 来自 FontAwesome 区段——全部并入（约 60+ 字形，体积可控）
hdr = "simulator/lvgl-pc/build/_deps/lvgl-src/src/font/lv_symbol_def.h"
out = set()
with open(hdr, encoding="utf-8", errors="ignore") as f:
    for m in re.finditer(r'#define\s+LV_SYMBOL_\w+\s+"((?:\\x[0-9A-Fa-f]{2})+)"', f.read()):
        raw = bytes(int(b, 16) for b in re.findall(r'\\x([0-9A-Fa-f]{2})', m.group(1)))
        try:
            out.add(raw.decode("utf-8"))
        except UnicodeDecodeError:
            pass
sys.stdout.write("".join(sorted(out)))
PYEOF
)
echo "   CJK/全角 ${#SYMBOLS} 个 · symbol ${#FA_SYMBOLS} 个"

mkdir -p "$OUT_DIR"
for sz in $SIZES; do
  echo "== 生成 ${sz}px"
  npx --yes lv_font_conv \
    --font "$FONT_TTF" \
    --range "0x20-0x7F" \
    --symbols "$SYMBOLS" \
    --font "$FA_WOFF" \
    --symbols "$FA_SYMBOLS" \
    --size "$sz" \
    --bpp 4 \
    --no-compress \
    --no-kerning \
    --format lvgl \
    --lv-include "lvgl.h" \
    --lv-font-name "lv_font_cn_${sz}" \
    -o "$OUT_DIR/font_cn_${sz}.c"
done

echo "完成：$OUT_DIR/font_cn_{12,14,16,20}.c（请重编模拟器）"
