#!/bin/sh
# 本机启动 Bridge（凭据存储开启）。秘密在 bridge.env（gitignored）：
#   BRIDGE_MASTER_KEY=<64 hex>（credential store AES 加密主钥）
#   ZHIPU_API_KEY=...（可选：env 直配智谱 Key）
cd "$(dirname "$0")"
[ -f bridge.env ] && . ./bridge.env
pkill -f "gadget-bridge -addr" 2>/dev/null
sleep 1
nohup ./gadget-bridge -addr :8081 >> /tmp/bridge.log 2>&1 &
sleep 2
tail -1 /tmp/bridge.log
