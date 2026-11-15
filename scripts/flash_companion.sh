#!/bin/bash
# flash_companion.sh — 把伴宠资源包烧到 assets 分区（0xC10000）。
# 前置：scripts/gen_companion_bin.sh 已生成 firmware/build/companion_assets.bin。
# 分区偏移来自 firmware/partitions.csv 的 assets 行，改表要同步改这里。
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=firmware/build/companion_assets.bin
[ -f "$BIN" ] || { echo "缺 $BIN，先跑 scripts/gen_companion_bin.sh"; exit 1; }

PORT="${1:-/dev/cu.usbmodem1101}"
export PATH="$HOME/esp/idf-buildenv/bin:/opt/homebrew/bin:$PATH"
. "$HOME/esp/esp-idf-v6.0.3/export.sh" >/dev/null

# 释放串口（monitor 占用会让烧写失败）
lsof -t "$PORT" 2>/dev/null | xargs -r kill -9 2>/dev/null || true
sleep 1

python -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
  --before default-reset --after hard-reset write-flash 0xC10000 "$BIN"
echo "companion assets flashed @0xC10000"
