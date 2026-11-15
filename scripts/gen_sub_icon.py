#!/usr/bin/env python3
"""gen_sub_icon.py — 订阅 App 桌面图标（额度进度条风格）。

用户反馈：原图标是"金币+闪电"，重点错了——订阅页真正关心的是**剩余额度**。
新图标用三条进度条表达"额度/用量"：满格、过半、见底各一条。
输出 108×108（与 icon_embed.sh 的磁贴尺寸 1:1，PNG 不透明）。
"""
import os

from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "simulator/lvgl-pc/assets/icons/sub.png")
S = 108
SS = 4  # 超采样
W = S * SS


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def main():
    img = Image.new("RGB", (W, W), (0, 0, 0))
    d = ImageDraw.Draw(img)

    # 背景：深蓝绿渐变（圆角方形，磁贴本身还会再裁圆角）
    top, bot = (10, 26, 38), (16, 58, 62)
    for y in range(W):
        d.line([(0, y), (W, y)], fill=lerp(top, bot, y / W))

    # 内侧微光（左上高光）
    glow = Image.new("L", (W, W), 0)
    gd = ImageDraw.Draw(glow)
    gd.ellipse([-W // 3, -W // 2, W, W // 2], fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(W // 12))
    img = Image.composite(Image.new("RGB", (W, W), (120, 220, 210)), img, glow.point(lambda v: v // 2))
    d = ImageDraw.Draw(img)

    # 三条进度条：满格 / 62% / 34%
    pads = int(W * 0.16)
    bar_h = int(W * 0.115)
    gap = int(W * 0.085)
    total_h = bar_h * 3 + gap * 2
    y0 = (W - total_h) // 2
    bar_w = W - pads * 2
    r = bar_h // 2
    fills = (1.0, 0.62, 0.34)
    c1, c2 = (46, 196, 182), (126, 231, 149)  # 青 → 嫩绿

    for i, frac in enumerate(fills):
        y = y0 + i * (bar_h + gap)
        # 轨道
        d.rounded_rectangle([pads, y, pads + bar_w, y + bar_h], radius=r,
                            fill=(255, 255, 255, 40) if img.mode == "RGBA" else (38, 74, 78))
        # 填充
        fw = max(bar_h, int(bar_w * frac))
        seg = Image.new("RGB", (fw, bar_h))
        sd = ImageDraw.Draw(seg)
        for x in range(fw):
            sd.line([(x, 0), (x, bar_h)], fill=lerp(c1, c2, x / max(1, bar_w - 1)))
        mask = Image.new("L", (fw, bar_h), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, fw - 1, bar_h - 1], radius=r, fill=255)
        img.paste(seg, (pads, y), mask)

    # 整图圆角裁切（圆角半径与磁贴一致）
    radius = int(W * 0.22)
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=radius, fill=255)
    out = Image.new("RGB", (W, W), (0, 0, 0))
    out.paste(img, (0, 0), mask)
    out = out.resize((S, S), Image.LANCZOS)
    out.save(OUT)
    print(f"{OUT}: {S}x{S}")


if __name__ == "__main__":
    main()
