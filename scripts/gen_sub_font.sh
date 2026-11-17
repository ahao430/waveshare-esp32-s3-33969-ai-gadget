#!/usr/bin/env bash
# 生成订阅页加粗中文字体（Noto Sans SC wght=700 → font_sub_12/14/16）。
#
# 背景（2026-11-17 用户反馈）：订阅页小字沿用 font_cn_*（Regular 400），
# AMOLED 上"很细看不清"；番茄钟已换 wght=700 加粗验收通过，订阅页照做。
#
# 字符集=GB2312 全集（6763 字）：订阅页标题/套餐徽章文本来自 Bridge
# 数据（实例名/套餐名是任意动态中文），只扫页面源码字面量会豆腐（实测
# mock"智谱 Coding Plan"的"谱"出豆腐块）。全集策略同 gen_cn_font.sh。
# 另混入 FontAwesome LV_SYMBOL_*（同 gen_cn_font.sh）。
#
# 同尺寸直接换粗体（不加字号，布局零风险）。生成参数与 gen_cn_font.sh
# 一致（RLE 压缩、bpp4、锁 lv_font_conv@1.5.3——不锁版本会因生成器
# 格式漂移重写全部 .c）。
#
# 依赖：npx lv_font_conv + fonttools（首次自动建 venv）。
# 字体源：/tmp/cnfont/NotoSansSC.ttf（variable wght；gen_cn_font.sh 会下载）。
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_TTF="/tmp/cnfont/NotoSansSC.ttf"
BOLD_TTF="${BOLD_TTF:-/tmp/cnfont/NotoSansSC-Bold.ttf}"
FA_WOFF="simulator/lvgl-pc/build/_deps/lvgl-src/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
OUT_DIR="${OUT_DIR:-simulator/lvgl-pc/src/ui/fonts}"
SIZES="12 14 16"

if [ ! -f "$FONT_TTF" ]; then
  echo "错误：未找到 $FONT_TTF（先运行 scripts/gen_cn_font.sh 下载 Noto Sans SC）" >&2
  exit 1
fi
if [ ! -f "$FA_WOFF" ]; then
  echo "错误：未找到 FontAwesome 字体（$FA_WOFF）——先完成模拟器首次构建" >&2
  exit 1
fi

# fonttools：优先当前 python3，否则临时 venv（仅首次联网安装）
PYBIN=python3
if ! $PYBIN -c "import fontTools" >/dev/null 2>&1; then
  VENV="${TMPDIR:-/tmp}/pomo-font-venv"
  echo "== 初始化 fonttools venv：$VENV"
  [ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
  "$VENV/bin/pip" -q install fonttools
  PYBIN="$VENV/bin/python"
fi

if [ ! -f "$BOLD_TTF" ]; then
  echo "== 实例化 wght=700 -> $BOLD_TTF"
  "$PYBIN" - "$FONT_TTF" "$BOLD_TTF" <<'PY'
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

src, dst = sys.argv[1], sys.argv[2]
font = TTFont(src)
inst = instantiateVariableFont(font, {"wght": 700})
inst.save(dst)
print("saved", dst)
PY
fi

# 字符集：GB2312 全集（区位 16-87 = 汉字全集）+ 页面字面量 + 全角标点
SYMBOLS=$(python3 - <<'PY'
import re, sys
chars = set()
with open("simulator/lvgl-pc/src/ui/ui_app_pages.c", encoding="utf-8") as f:
    src = f.read()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    for ch in lit:
        if ord(ch) > 0x7E:
            chars.add(ch)
chars |= set("·，。！？：；（）—…")
# GB2312 全集：区位 16-87 区 = 汉字全集（一级+二级，6763 字）
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.add(bytes([hi, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
sys.stdout.write("".join(sorted(chars)))
PY
)

# LV_SYMBOL_*（FontAwesome 私有区），做法同 gen_cn_font.sh
FA_SYMBOLS=$(python3 - <<'PYEOF'
import re, sys
hdr = "simulator/lvgl-pc/build/_deps/lvgl-src/src/font/lv_symbol_def.h"
out = set()
with open(hdr, encoding="utf-8", errors="ignore") as p:
    for m in re.finditer(r'#define\s+LV_SYMBOL_\w+\s+"((?:\\x[0-9A-Fa-f]{2})+)"', p.read()):
        raw = bytes(int(b, 16) for b in re.findall(r'\\x([0-9A-Fa-f]{2})', m.group(1)))
        try:
            out.add(raw.decode("utf-8"))
        except UnicodeDecodeError:
            pass
sys.stdout.write("".join(sorted(out)))
PYEOF
)

echo "== 订阅页字符集（GB2312 全集+页面字面量）：$(printf '%s' "$SYMBOLS" | wc -m | tr -d ' ') 个 · FA ${#FA_SYMBOLS} 个"
mkdir -p "$OUT_DIR"
for sz in $SIZES; do
  echo "== 生成 ${sz}px 加粗"
  npx --yes lv_font_conv@1.5.3 \
    --font "$BOLD_TTF" \
    --range "0x20-0x7F" \
    --symbols "$SYMBOLS" \
    --font "$FA_WOFF" \
    --symbols "$FA_SYMBOLS" \
    --size "$sz" --bpp 4 --no-kerning \
    --format lvgl --lv-include "lvgl.h" \
    --lv-font-name "font_sub_${sz}" \
    -o "$OUT_DIR/font_sub_${sz}.c"
done

echo "完成：$OUT_DIR/font_sub_{12,14,16}.c（请重编模拟器/固件）"
