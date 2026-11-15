#!/usr/bin/env bash
# 全仓库快速检查：能构建的都构建一遍（固件除外，本机无 IDF）。
set -euo pipefail
cd "$(dirname "$0")/.."

echo "=== Bridge (Go) ==="
(cd bridge && go vet ./... && go build ./... && go test ./...)

echo "=== Web (Vue3+TS) ==="
(cd web && [ -d node_modules ] || npm install --silent)
(cd web && npm run typecheck)

echo "=== Simulator (LVGL PC) ==="
if [ ! -f simulator/lvgl-pc/build/gadget_sim ]; then
  cmake -S simulator/lvgl-pc -B simulator/lvgl-pc/build >/dev/null
fi
cmake --build simulator/lvgl-pc/build -j >/dev/null
echo "simulator binary: $(ls simulator/lvgl-pc/build/gadget_sim)"

echo "=== Firmware (ESP-IDF) ==="
IDF_DIR="$HOME/esp/esp-idf-v6.0.3"
APP_PART_BYTES=$((4 * 1024 * 1024)) # factory 分区 4MB（partitions.csv）
if [ -d "$IDF_DIR" ]; then
  (
    export PATH="$HOME/esp/idf-buildenv/bin:/opt/homebrew/bin:$PATH"
    # shellcheck disable=SC1091
    . "$IDF_DIR/export.sh" >/dev/null
    cd firmware && idf.py build
  )
  # flash 余量守护：字体/素材很容易把 app 分区吃到只剩几 KB，
  # overflow 时 idf.py 会直接失败，但"刚好塞下"也要提前告警。
  BIN=firmware/build/ai_gadget.bin
  if [ -f "$BIN" ]; then
    SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
    FREE=$((APP_PART_BYTES - SIZE))
    PCT=$((FREE * 100 / APP_PART_BYTES))
    echo "app partition: ${SIZE} / ${APP_PART_BYTES} bytes · free ${FREE} (${PCT}%)"
    if [ "$PCT" -lt 5 ]; then
      echo "警告：app 分区余量 ${PCT}% < 5%——先做字体压缩（gen_cn_font.sh 默认）/裁剪素材，再考虑加功能" >&2
    fi
  fi
else
  echo "SKIP: $IDF_DIR 不存在（见 docs/DEVELOPMENT.md 环境搭建）"
fi

echo "=== ALL CHECKS PASSED ==="
