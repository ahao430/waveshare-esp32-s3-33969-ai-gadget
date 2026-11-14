#!/usr/bin/env bash
# companion_gif2frames — 过渡期素材管线：现有 GIF → 带 alpha 的 PNG 序列帧。
#
# 背景：gifdec 的调色板透明在本项目两次渲染成绿色（素材/调色板相关），
# 且 GIF 256 色量化使合成底色与页面底存在色差。PNG（RGBA）是 LVGL
# 原生透明路径，且输出目录对齐 docs/COMPANION_ASSETS.md §2 的
# companions/<char>/<action>/frame_NN.png 规范——将来美术逐帧素材
# 直接替换同名文件即可，播放器不动。
#
# 用法：scripts/companion_gif2frames.sh   （输入输出表见下方 TABLE）
set -euo pipefail
cd "$(dirname "$0")/.."

# 输入 gif | 角色/动作 | colorkey（None=源已带透明；key 值=当前 gif 的
# 合成实底色——见 git 历史的 overlay 处理）
TABLE=(
  "simulator/lvgl-pc/assets/dsh/dsh-idle.gif|bigfish/idle|0x111A33:0.10"
  "simulator/lvgl-pc/assets/dsh/dsh-typing.gif|bigfish/thinking|0x111A33:0.10"
  "simulator/lvgl-pc/assets/dsh/dsh-sparkle.gif|bigfish/speaking|0x111A33:0.10"
  "simulator/lvgl-pc/assets/xiaozhi/xiaozhi-wave.gif|xiaozhi/wave|0x0B1020:0.14"
  "simulator/lvgl-pc/assets/mascot.gif|muse/idle|none"
)

for row in "${TABLE[@]}"; do
  IFS='|' read -r src dst key <<<"$row"
  out="simulator/lvgl-pc/assets/companions/$dst"
  mkdir -p "$out"
  rm -f "$out"/frame_*.png

  # colorkey=颜色:相似度（近底色→alpha 0），blend 0.15 软化边缘
  if [[ "$key" == "none" ]]; then
    vf="fps=8"
  else
    color="${key%%:*}"; sim="${key##*:}"
    vf="colorkey=${color}:${sim}:0.15,fps=8"
  fi

  ffmpeg -y -v error -i "$src" -vf "$vf" -start_number 0 "$out/frame_%02d.png"
  n=$(ls "$out"/frame_*.png | wc -l | tr -d ' ')
  echo "$dst ← $(basename "$src")：${n} 帧（${key}）"
done

echo "完成：simulator/lvgl-pc/assets/companions/<char>/<action>/frame_NN.png"
