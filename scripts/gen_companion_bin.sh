#!/bin/bash
# gen_companion_bin.sh — 伴宠帧动画 → 固件 assets 分区资源包（真机）。
#
# 模拟器直接读 assets/companions/<dir>/frame_NN.png（文件系统）；
# 真机没有文件系统，本脚本把帧序列打包成 RGB565A8 原生数组blob：
#   [magic "CMP1"][u32 条目数]
#   条目×N（32B）: key[16] u32 w h frames frame_bytes offset
#   帧数据：RGB565 平面 (w*h*2) + A8 平面 (w*h)——LVGL 原生格式零解码
# 帧数/尺寸预算（assets 分区 2.5MB）：
#   bigfish 三动作 144×82 原生 ×12 帧 ≈ 1.28MB
#   xiaozhi/wave  132×117（0.6 缩）×10 帧 ≈ 0.46MB
#   muse/idle     150×134（0.34 缩）×10 帧 ≈ 0.60MB
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
    ("bigfish/idle",     144,  82, 12),
    ("bigfish/thinking", 144,  82, 12),
    ("bigfish/speaking", 144,  82, 12),
    ("xiaozhi/wave",     132, 117, 10),
    ("muse/idle",        150, 134, 10),
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
hdr_len = 8 + 32 * len(SPECS)
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
    ekey = key.encode()[:15]
    entries.append(struct.pack("<16sIIIII", ekey, tw, th, keep, fbytes,
                               hdr_len + offset))
    blobs += frames
    offset += len(frames)
    print(f"{key:18s} {tw}x{th} {keep}帧 {fbytes*keep//1024}KB")

blob = b"CMP1" + struct.pack("<I", len(SPECS)) + b"".join(entries) + blobs
with open(out, "wb") as f:
    f.write(blob)
print(f"\n{out}: {len(blob)} bytes ({len(blob)/1024/1024:.2f}MB, 分区预算 2.5MB)")
assert len(blob) <= 2560 * 1024, "超过 assets 分区预算"
PY
