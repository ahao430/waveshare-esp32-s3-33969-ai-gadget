#!/usr/bin/env bash
# 生成番茄钟专用加粗中文字体（Noto Sans SC wght=700 → font_pomo_20）。
#
# 为什么单独生成：
#   - 界面文字要"大、亮、粗"（2026-11-16 用户反馈），现有 font_cn_*
#     是 Regular 400，20px 也不够厚重；
#   - 只扫 ui_pomodoro.c 的字符（约 50 个 CJK），两档字号合计很小；
#   - 不重跑 gen_cn_font.sh（那会重生成整个 GB2312 字库，慢且无必要）。
#
# 依赖：npx lv_font_conv（同 gen_cn_font.sh）+ fonttools（首次自动建 venv）。
# 字体源：/tmp/cnfont/NotoSansSC.ttf（variable wght；gen_cn_font.sh 会下载）。
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_TTF="${FONT_TTF:-/tmp/cnfont/NotoSansSC.ttf}"
BOLD_TTF="${BOLD_TTF:-/tmp/cnfont/NotoSansSC-Bold.ttf}"
OUT_DIR="${OUT_DIR:-simulator/lvgl-pc/src/ui/fonts}"

if [ ! -f "$FONT_TTF" ]; then
  echo "错误：未找到 $FONT_TTF（先运行 scripts/gen_cn_font.sh 下载 Noto Sans SC）" >&2
  exit 1
fi

# fonttools：优先当前 python3，否则临时 venv（仅首次联网安装）
PYBIN=python3
if ! $PYBIN -c "import fontTools" >/dev/null 2>&1; then
  VENV="${TMPDIR:-/tmp}/pomo-font-venv"
  echo "== 初始化 fonttools venv：$VENV"
  [ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q fonttools
  PYBIN="$VENV/bin/python"
fi

if [ ! -f "$BOLD_TTF" ]; then
  echo "== 实例化 wght=700 → $BOLD_TTF"
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

# 字符集：ui_pomodoro.c 字符串字面量里的全部非 ASCII（不含注释）
SYMBOLS=$(python3 - <<'PY'
import re, sys
chars = set()
with open("simulator/lvgl-pc/src/ui/ui_pomodoro.c", encoding="utf-8") as f:
    src = f.read()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    for ch in lit:
        if ord(ch) > 0x7E:
            chars.add(ch)
chars |= set("·，。！？：；（）—…")
sys.stdout.write("".join(sorted(chars)))
PY
)

echo "== 番茄钟字符集：$(printf '%s' "$SYMBOLS" | wc -m | tr -d ' ') 个 CJK/符号"
mkdir -p "$OUT_DIR"
for sz in 20; do
  echo "== 生成 ${sz}px 加粗"
  npx --yes lv_font_conv \
    --font "$BOLD_TTF" \
    --range "0x20-0x7E" \
    --symbols "$SYMBOLS" \
    --size "$sz" --bpp 4 --no-kerning \
    --format lvgl --lv-include "lvgl.h" \
    --lv-font-name "font_pomo_${sz}" \
    -o "$OUT_DIR/font_pomo_${sz}.c"
done

echo "完成：$OUT_DIR/font_pomo_20.c（请重编模拟器/固件）"