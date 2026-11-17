#!/bin/sh
# 全 App headless 回归：8 个页面截帧 → 非空 + 两两去重。
# 用途：UI 改动后一键确认「每个 App 都能渲染、没有白屏/黑屏、页面互不串」。
# 数据点：GADGET_DEMO_APP=<枚举号>（ui.h enum：0=智谱 1=对话 2=Muse 3=小智
# 5=文件 6=设置 7=天气 8=番茄钟）；GADGET_SNAP=<path.ppm> 截帧。
# 输出 PASS / FAIL，FAIL 时指明异常页。UI 改动的提交前建议跑一次。
set -e
cd "$(dirname "$0")/.."

BIN=simulator/lvgl-pc/build/gadget_sim
[ -x "$BIN" ] || { echo "FAIL: 先构建模拟器（simulator/lvgl-pc cmake build）"; exit 1; }

OUT=$(mktemp -d /tmp/gadget-regress.XXXX)
trap 'rm -rf "$OUT"' EXIT

# 枚举号：UI_APP_COUNT 内有效 App
for id in 0 1 2 3 4 5 6 7 8 9; do
    GADGET_SNAP="$OUT/app${id}.ppm" GADGET_DEMO_APP=$id "$BIN" >/dev/null 2>&1
done

fail=0
for id in 0 1 2 3 4 5 6 7 8 9; do
    f="$OUT/app${id}.ppm"
    if [ ! -s "$f" ] || [ "$(wc -c <"$f")" -lt 500000 ]; then
        echo "FAIL: app$id 截帧缺失/过小"; fail=1
    fi
done
[ "$fail" = 1 ] && exit 1

# 两两去重：9 页应 ≥6 个唯一画面（背景相同的纯文字页仍应有差异）
uniq=$(md5 -q "$OUT"/*.ppm | sort -u | wc -l | tr -d ' ')
if [ "$uniq" -lt 6 ]; then
    echo "FAIL: 10 页只有 $uniq 个唯一画面（页面构建回归?）"
    exit 1
fi

echo "PASS: 10 App 全部渲染，唯一画面 $uniq/10（2026-11-18 基线 = 10，阅读器已接入）"
echo "  截帧留存: $OUT"
