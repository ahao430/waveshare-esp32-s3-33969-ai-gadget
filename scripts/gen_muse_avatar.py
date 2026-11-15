#!/usr/bin/env python3
"""gen_muse_avatar.py — 从 Muse 官方 jollybot.gif 生成伴宠帧。

Muse 页面要显示的是 Muse 自己的形象（官方固件屏上那只像素小绒球），
不是早期概念图的蓝机器人。上游素材：
  <muse-gadget-sdk>/esp32/avatar/jollybot.gif  (320×320, 75 帧)

处理：黑底→透明（从边缘 flood fill 去背景，保留角色内部深色描边）、
按所有帧的并集 bbox 裁切、等比缩放到 SIDE、均匀抽 FRAMES 帧、写 PNG。

用法：
  scripts/gen_muse_avatar.py [--sdk ~/esp/muse-gadget-sdk] \
      [--frames 12] [--side 140] [--out simulator/lvgl-pc/assets/companions/muse/idle]
"""
import argparse
import os
import shutil
import sys
from collections import deque

from PIL import Image, ImageSequence

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def strip_black_bg(im: Image.Image, thresh: int = 14) -> Image.Image:
    """从四边 flood fill 掉近黑背景（角色内部深色像素不受影响）。"""
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    seen = bytearray(w * h)
    q = deque()

    def dark(x, y):
        r, g, b, a = px[x, y]
        return a > 0 and (0.2126 * r + 0.7152 * g + 0.0722 * b) < thresh

    for x in range(w):
        for y in (0, h - 1):
            if not seen[y * w + x] and dark(x, y):
                seen[y * w + x] = 1
                q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if not seen[y * w + x] and dark(x, y):
                seen[y * w + x] = 1
                q.append((x, y))
    while q:
        x, y = q.popleft()
        px[x, y] = (0, 0, 0, 0)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and dark(nx, ny):
                seen[ny * w + nx] = 1
                q.append((nx, ny))
    return im


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", default=os.path.expanduser("~/esp/muse-gadget-sdk"))
    ap.add_argument("--frames", type=int, default=12)
    ap.add_argument("--side", type=int, default=140)
    ap.add_argument("--out", default=os.path.join(
        ROOT, "simulator/lvgl-pc/assets/companions/muse/idle"))
    args = ap.parse_args()

    gif = os.path.join(args.sdk, "esp32/avatar/jollybot.gif")
    if not os.path.exists(gif):
        print(f"缺 {gif}（clone facebookincubator/muse-gadget-sdk）", file=sys.stderr)
        return 1

    src = Image.open(gif)
    frames = [strip_black_bg(f) for f in ImageSequence.Iterator(src)]
    if not frames:
        print("GIF 没有帧", file=sys.stderr)
        return 1

    # 所有帧的并集 bbox（避免逐帧裁切导致抖动）
    box = None
    for f in frames:
        b = f.getchannel("A").getbbox()
        if b:
            box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]),
                                         max(box[2], b[2]), max(box[3], b[3]))
    if not box:
        print("去背景后没有内容", file=sys.stderr)
        return 1
    # 扩一点边，再方形化
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    half = max(x1 - x0, y1 - y0) // 2 + 6
    box = (max(0, cx - half), max(0, cy - half), min(src.width, cx + half), min(src.height, cy + half))

    n = len(frames)
    keep = args.frames
    idxs = [round(i * (n - 1) / (keep - 1)) for i in range(keep)] if keep > 1 else [0]

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)
    for k, i in enumerate(idxs):
        im = frames[i].crop(box)
        # 预乘 alpha 再缩放，避免黑边
        im = im.convert("RGBa").resize((args.side, args.side), Image.LANCZOS).convert("RGBA")
        im.save(os.path.join(args.out, f"frame_{k:02d}.png"))
    print(f"{args.out}: {keep} 帧 {args.side}×{args.side}（源 {n} 帧 {src.size}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
