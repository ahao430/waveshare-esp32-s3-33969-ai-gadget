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
#   默认生成 RLE 压缩字体（LVGL 原生解码，体积约为位图一半；需
#   LV_USE_FONT_COMPRESSED=1，固件/模拟器均已开）。COMPRESS=0 → 位图。
#   OUT_DIR=<dir> → 输出到别处（AB 对比体积用）。
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_TTF="${FONT_TTF:-/tmp/cnfont/NotoSansSC.ttf}"
FA_WOFF="simulator/lvgl-pc/build/_deps/lvgl-src/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
OUT_DIR="${OUT_DIR:-simulator/lvgl-pc/src/ui/fonts}"
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

# lv_font_conv 默认就开 RLE 压缩；--no-compress 才是关。COMPRESS=1 表示
# 走默认（压缩），否则显式关闭（历史行为，位图直取）
# 默认压缩（flash 预算紧：未压缩时 app 分区仅剩 2%）；COMPRESS=0 可关
if [ "${COMPRESS:-1}" = "1" ]; then COMPRESS_FLAG=""; else COMPRESS_FLAG="--no-compress"; fi
echo "== 字体模式：${COMPRESS_FLAG}"

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
chars |= set("（）【】·％、，。：；！？°")

# 天气描述字典（bridge internal/weather codeTable 全集——服务器下发的
# 动态描述必须提前入集，否则设备豆腐）
chars |= set("晴间少云局部阴有雾冻毛毛雨浓小中大雪粒阵强雷伴冰雹雨"
             "体感湿度风更新获取失败服务未连接或定位可达点重试天气")

# GB2312 一级字库（3755 常用汉字）：覆盖城市/行政区名（服务器下发、
# 源码里不会出现的动态中文）。区位 16-55 区即汉字一级区。
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            ch = bytes([hi, lo]).decode("gb2312")
            chars.add(ch)
        except UnicodeDecodeError:
            pass
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
    $COMPRESS_FLAG \
    --no-kerning \
    --format lvgl \
    --lv-include "lvgl.h" \
    --lv-font-name "lv_font_cn_${sz}" \
    -o "$OUT_DIR/font_cn_${sz}.c"
done

# ---- 动态中文回退字体（DYN=1 才生成）----
# 默认关闭：用户 2026-11-15 决策"只保留常用字"（GB2312 一级），
# 代价是自定义机器人名里的生僻字（魅/薰）显示豆腐。
# 机器人名等"服务器/用户自定义"中文不受源码字面量约束，常含 GB2312
# 一级区以外的字（如实测：魅=GBK f7c8、薰=deb9）→ 单列 GBK 二级汉字
# 区（0xD8-0xF7）小字体，14px / bpp2（约 150KB，flash 预算内），挂在
# 16/20px 主字体的 fallback 上：主字体没有的字自动由它渲染，不再豆腐。
if [ "${DYN:-0}" != "1" ]; then
  echo "== 跳过动态回退字体（DYN=1 可启用）"
  # 从主字体里摘掉 fallback（幂等）
  python3 - "$OUT_DIR" <<'PYEOF2'
import sys, pathlib, re
out = pathlib.Path(sys.argv[1])
for size in ("16", "20"):
    p = out / f"font_cn_{size}.c"
    if not p.exists():
        continue
    s = p.read_text(encoding="utf-8")
    s = re.sub(r"extern const lv_font_t lv_font_cn_dyn_14;[^\n]*\n\n", "", s)
    s = s.replace(".fallback = &lv_font_cn_dyn_14,", ".fallback = NULL,")
    p.write_text(s, encoding="utf-8")
PYEOF2
  echo "完成：$OUT_DIR/font_cn_{12,14,16,20}.c（无动态回退，请重编模拟器）"
  exit 0
fi

echo "== 生成动态名回退字体（GBK 二级区，14px bpp2）"
DYNSYMS=$(python3 - <<'PYEOF'
import sys
chars = set()
# 严格走 gb2312 解码：只收 GB2312 二级汉字（3008 个简体字）。
# 若用 gbk 解码同一区间会混入 7000+ 繁体/扩展字，固件超 105KB（实测）。
for hi in range(0xD8, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.add(bytes([hi, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
sys.stdout.write("".join(sorted(chars)))
PYEOF
)
npx --yes lv_font_conv \
  --font "$FONT_TTF" \
  --symbols "$DYNSYMS" \
  --size 14 --bpp 2 $COMPRESS_FLAG --no-kerning \
  --format lvgl --lv-include "lvgl.h" \
  --lv-font-name "lv_font_cn_dyn_14" \
  -o "$OUT_DIR/font_cn_dyn_14.c"

echo "== 给 16/20px 挂 fallback"
python3 - "$OUT_DIR" <<'PYEOF'
import sys, pathlib
out = pathlib.Path(sys.argv[1])
for size in ("16", "20"):
    p = out / f"font_cn_{size}.c"
    s = p.read_text(encoding="utf-8")
    if "lv_font_cn_dyn_14" in s:
        continue
    # extern 必须在 #ifdef LV_LVGL_H_INCLUDE_SIMPLE 之外：固件走 #else
    # 分支，写进 #ifdef 里声明不可见（真机编译 'undeclared' 实测）
    # 必须在 include 之后（否则 lv_font_t 未定义）、且不放进
    # #ifdef LV_LVGL_H_INCLUDE_SIMPLE（固件走 #else 分支不可见）
    s = s.replace(f'#ifndef LV_FONT_CN_{size}',
                  f'extern const lv_font_t lv_font_cn_dyn_14; /* 动态中文回退 */\n\n#ifndef LV_FONT_CN_{size}', 1)
    # 生成器在结构体末尾自带 `.fallback = NULL,`——必须替换而不是在开头
    # 插入（后写的字段覆盖前面，插在开头会被 NULL 覆盖 → 回退不生效）
    s = s.replace(".fallback = NULL,",
                  ".fallback = &lv_font_cn_dyn_14,")
    p.write_text(s, encoding="utf-8")
    print(f"  patched font_cn_{size}.c")
PYEOF

echo "完成：$OUT_DIR/font_cn_{12,14,16,20}.c + font_cn_dyn_14.c（请重编模拟器）"
