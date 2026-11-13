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
if [ -d "$IDF_DIR" ]; then
  (
    export PATH="$HOME/esp/idf-buildenv/bin:/opt/homebrew/bin:$PATH"
    # shellcheck disable=SC1091
    . "$IDF_DIR/export.sh" >/dev/null
    cd firmware && idf.py build
  )
else
  echo "SKIP: $IDF_DIR 不存在（见 docs/DEVELOPMENT.md 环境搭建）"
fi

echo "=== ALL CHECKS PASSED ==="
