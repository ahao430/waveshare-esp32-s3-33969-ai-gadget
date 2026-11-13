#!/usr/bin/env bash
# 预置 lvgl 组件到 managed_components/（绕过组件管理器对 115MB 大包的下载卡死）。
#
# 背景（详见 docs/DEVELOPMENT.md）：
#   - lvgl 9.6.0~1 组件包 ~115MB；idf 组件管理器下载时长期卡死
#     （components-file.espressif.com → .com.cn 重定向），curl -L 手动下载 9 秒完成。
#   - 直接解压进 managed_components 后，还需生成 CHECKSUMS.json 与
#     .component_hash（用组件管理器自带的 hash_tools），否则完整性检查报错。
#   - override_path 方案不可行：esp_lvgl_adapter 硬编码查找组件名 lvgl__lvgl。
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:-9.6.0~1}"
DEST="firmware/managed_components/lvgl__lvgl"
FILE_VER="${VER//\~/_}"              # 9.6.0~1 → 9.6.0_1
URL="https://components-file.espressif.com/components/lvgl/lvgl/${VER}/lvgl__lvgl-v${FILE_VER}.zip"
IDF_PY="${IDF_PYTHON:-$HOME/.espressif/python_env/idf6.0_py3.14_env/bin/python}"

TMP="$(mktemp -d)/lvgl.zip"
echo "== 下载 ${URL}"
curl -fL --retry 3 -o "$TMP" "$URL"

echo "== 解压到 ${DEST}"
mkdir -p "$DEST"
rm -rf "${DEST:?}"/*
unzip -q -o "$TMP" -d "$DEST"

echo "== 生成完整性文件（CHECKSUMS.json + .component_hash）"
"$IDF_PY" - <<'PYEOF'
from pathlib import Path
from idf_component_tools.hash_tools.calculate import hash_dir
from idf_component_tools.hash_tools.checksums import ChecksumsManager
d = Path("firmware/managed_components/lvgl__lvgl")
ChecksumsManager(d).dump()
(d / ".component_hash").write_text(hash_dir(d))
print("hash ok")
PYEOF

echo "完成：lvgl $(grep -m1 '^version' "$DEST/idf_component.yml")"
