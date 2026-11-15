#!/bin/bash
# gen_companion_bin.sh — 伴宠帧动画 → 固件 assets 分区资源包（真机）。
#
# 模拟器直接读 assets/companions/<dir>/frame_NN.png（文件系统）；
# 真机没有文件系统，本脚本把帧序列打包成 RGB565A8 原生数组blob：
#   [magic "CMP1"][u32 条目数]
#   条目×N（44B）: key[24] u32 w h frames frame_bytes offset
#   （key 24 字节：16 字节放不下 "bigfish/thinking"——曾被截成
#     "bigfish/thinkin"，strncmp 永不匹配，thinking/speaking 一直退化成 idle）
#   帧数据：RGB565 平面 (w*h*2) + A8 平面 (w*h)——LVGL 原生格式零解码
# 帧数/尺寸预算（assets 分区 2.5MB）：
#   bigfish 三动作 200×114（源 144×82 离线 LANCZOS 放大）×12 帧 ≈ 2.4MB
#   xiaozhi/wave  132×117（0.6 缩）×10 帧 ≈ 0.46MB
#   muse/idle     140×140（官方 jollybot）×12 帧 ≈ 0.69MB
# 烧写：scripts/flash_companion.sh（0xC10000）。
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT=simulator/lvgl-pc/assets/companions
OUT=firmware/build/companion_assets.bin
mkdir -p firmware/build

python3 - "$ROOT" "$OUT" <<'PY'
import struct, sys, glob
from PIL import Image

root, out = sys.argv[1], sys.argv[2]

# (key=目录相对路径, 目标宽, 目标高, 保留帧数)
SPECS = [
    ("bigfish/idle",     200, 114, 12),
    ("bigfish/thinking", 200, 114, 12),
    ("bigfish/speaking", 200, 114, 12),
    ("xiaozhi/wave",     132, 117, 10),
    # Muse 用官方 jollybot 形象（scripts/gen_muse_avatar.py 生成源帧）
    ("muse/idle",        140, 140, 12),
]

def rgb565a8(im):
    """RGBA PIL → RGB565LE 平面 + A8 平面（LVGL LV_COLOR_FORMAT_RGB565A8）"""
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    plane = bytearray(w * h * 2)
    alpha = bytearray(w * h)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            rgb = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            plane[i * 2] = rgb & 0xFF
            plane[i * 2 + 1] = rgb >> 8
            alpha[i] = a
            i += 1
    return bytes(plane) + bytes(alpha)

entries, blobs, offset = [], b"", 0
hdr_len = 8 + 44 * len(SPECS)
for key, tw, th, keep in SPECS:
    files = sorted(glob.glob(f"{root}/{key}/frame_*.png"))
    if not files:
        sys.exit(f"missing frames: {key}")
    # 均匀抽帧（含首帧）
    idxs = [round(i * (len(files) - 1) / (keep - 1)) for i in range(keep)]
    frames = b""
    for i in idxs:
        im = Image.open(files[i])
        if im.size != (tw, th):
            im = im.resize((tw, th), Image.LANCZOS)
        frames += rgb565a8(im)
    fbytes = tw * th * 3
    assert len(frames) == fbytes * keep, (key, len(frames), fbytes * keep)
    ekey = key.encode()[:23]
    entries.append(struct.pack("<24sIIIII", ekey, tw, th, keep, fbytes,
                               hdr_len + offset))
    blobs += frames
    offset += len(frames)
    print(f"{key:18s} {tw}x{th} {keep}帧 {fbytes*keep//1024}KB")

blob = b"CMP1" + struct.pack("<I", len(SPECS)) + b"".join(entries) + blobs
with open(out, "wb") as f:
    f.write(blob)
print(f"\n{out}: {len(blob)} bytes ({len(blob)/1024/1024:.2f}MB, 分区预算 3.5MB)")
assert len(blob) <= 3584 * 1024, "超过 assets 分区预算"
PY
