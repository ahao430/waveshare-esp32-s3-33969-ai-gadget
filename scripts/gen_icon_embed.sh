#!/bin/sh
# 桌面图标 PNG → 固件嵌入源（icon_embed.c/.h）。
# 真机无文件系统路径（"A:..." 仅模拟器协议），图标以 PNG 原始字节
# 编入固件，运行时包 lv_image_dsc_t 交给 esp_lv_decoder（libpng）解码。
# 改动 assets/icons/*.png 后重跑本脚本 + 重建固件。
set -e
cd "$(dirname "$0")/.."
SRC_DIR=simulator/lvgl-pc/assets/icons
OUT_DIR=simulator/lvgl-pc/src/ui/icons
mkdir -p "$OUT_DIR"

C_FILE="$OUT_DIR/icon_embed.c"
H_FILE="$OUT_DIR/icon_embed.h"

{
  echo "/* icon_embed — 自动生成，勿手改（scripts/gen_icon_embed.sh）。"
  echo " * 桌面图标 PNG 嵌入（esp_lv_decoder 内存解码；cf=UNKNOWN，"
  echo " * w/h 由 decoder_info 从 IHDR 解析）。模拟器走文件路径不经此文件。 */"
  echo "#include <string.h>"
  echo '#include "lvgl.h"'
  echo '#include "icon_embed.h"'
  echo
} > "$C_FILE"

names=""
for png in "$SRC_DIR"/*.png; do
  name=$(basename "$png" .png)
  names="$names $name"
  ( cd "$SRC_DIR" && xxd -i "$name.png" ) \
    | sed -e "s/^unsigned char ${name}_png/static const uint8_t icon_${name}_png/" \
          -e "s/^\(  \)/ \1/" >> "$C_FILE"
  cat >> "$C_FILE" <<CEOF

static const lv_image_dsc_t icon_${name}_dsc = {
    .header = { .cf = LV_COLOR_FORMAT_UNKNOWN },
    .data_size = sizeof(icon_${name}_png),
    .data = icon_${name}_png,
};
CEOF
done

{
  echo
  echo "const lv_image_dsc_t *ui_icon_find(const char *key)"
  echo "{"
  for name in $names; do
    echo "    if (strcmp(key, \"$name\") == 0) return &icon_${name}_dsc;"
  done
  echo "    return NULL;"
  echo "}"
} >> "$C_FILE"

{
  echo "/* icon_embed — 自动生成，勿手改。按文件名（无扩展名）查嵌入图标。 */"
  echo "#ifndef ICON_EMBED_H"
  echo "#define ICON_EMBED_H"
  echo
  echo '#include "lvgl.h"'
  echo
  echo "/* key=图标文件名（如 \"sub\"）；未嵌入返回 NULL（调用方回退占位）。 */"
  echo "const lv_image_dsc_t *ui_icon_find(const char *key);"
  echo
  echo "#endif /* ICON_EMBED_H */"
} > "$H_FILE"

total=$(du -ck "$SRC_DIR"/*.png | tail -1 | cut -f1)
echo "生成完成: $(echo $names | wc -w | tr -d ' ') 个图标，PNG 合计 ${total}KB"
