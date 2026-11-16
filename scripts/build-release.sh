#!/usr/bin/env bash
# build-release.sh — 部署打包（ADR-009）：
#   1. Web 构建成静态产物（web/dist → 打包为 webdist/）
#   2. Bridge 本机构建（darwin/arm64）
#   3. linux 交叉构建（可选：需 musl-cross 工具链，未装则跳过并提示——
#      目标机自建是等价路径，见 docs/DEPLOY.md）
# 产物：dist/gadget-bridge-<os>-<arch>[.tar.gz 内含 webdist]
set -euo pipefail
cd "$(dirname "$0")/.."
TAGS="nolibopusfile"
OUT=dist
rm -rf "$OUT"; mkdir -p "$OUT"

echo "== 1/3 web build =="
(cd web && npm install --no-audit --no-fund >/dev/null && npm run build)
cp -R web/dist "$OUT/webdist"

echo "== 2/3 bridge build (native) =="
(cd bridge && go build -tags "$TAGS" -o "../$OUT/gadget-bridge-$(go env GOOS)-$(go env GOARCH)" ./cmd/gadget-bridge)

echo "== 3/3 linux cross build (optional) =="
# hraban/opus 是 cgo：linux 交叉需 musl-gcc（brew install filosottile/musl-cross/musl-cross，
# 装 x86_64 与 aarch64 变体）。没有就跳过——在目标机上 go build 即可（DEPLOY.md §2）。
CROSS_BUILT=0
# 三字段：goos goarch cc 前缀（旧写法把 "linux/amd64" 当单字段，cc=$3
# 在 set -u 下必炸——2026-11-16 打包时踩到）
for tgt in "linux amd64 x86_64-linux-musl" "linux arm64 aarch64-linux-musl"; do
    set -- $tgt; goos=$1; goarch=$2; cc=$3
    if command -v "${cc}-gcc" >/dev/null 2>&1; then
        echo "  building $goos/$goarch ($cc)…"
        (cd bridge && CGO_ENABLED=1 CC="${cc}-gcc" GOOS=$goos GOARCH=$goarch \
            go build -tags "$TAGS" -o "../$OUT/gadget-bridge-$goos-$goarch" ./cmd/gadget-bridge)
        CROSS_BUILT=1
    fi
done
[ "$CROSS_BUILT" = 0 ] && echo "  （未检测到 musl-cross，跳过 linux 交叉——目标机自建见 docs/DEPLOY.md）"

echo "== 打包 =="
TAR="gadget-bridge-$(date +%Y%m%d).tar.gz"
tar -czf "$OUT/$TAR" -C "$OUT" webdist
echo "完成：$OUT/"
ls -lh "$OUT"
