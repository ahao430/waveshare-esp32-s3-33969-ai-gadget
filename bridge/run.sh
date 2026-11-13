#!/usr/bin/env bash
# Bridge 开发启动脚本：构建 + 单测 + 以 Mock Provider 启动。
set -euo pipefail
cd "$(dirname "$0")"

go vet ./...
go test ./...
echo "---- starting gadget-bridge on :8081 ----"
exec go run ./cmd/gadget-bridge -addr :8081
