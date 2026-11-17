#!/usr/bin/env python3
"""gen_gbk_table — GB2312→Unicode codepoint 解码矩阵（阅读器 TXT 换码）。

87 行(0xA1-0xF7) × 94 列(0xA1-0xFE)，每项 uint16 BMP codepoint，无效码位 0。
查询 O(1)：cp = gbk2312_cp[(hi-0xA1)*94 + (lo-0xA1)]。
体积 ~16KB rodata（坑 #25 口径已过账）。
"""
import pathlib

OUT = pathlib.Path("simulator/lvgl-pc/src/reader")
OUT.mkdir(parents=True, exist_ok=True)

cells = []
valid = 0
for hi in range(0xA1, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            cells.append(ord(bytes([hi, lo]).decode("gb2312")))
            valid += 1
        except UnicodeDecodeError:
            cells.append(0)

c = pathlib.Path(OUT / "gbk_table.c")
c.write_text(
    "/* gbk_table — GB2312→Unicode codepoint 解码矩阵（自动生成，勿手改；"
    "scripts/gen_gbk_table.py）。\n"
    " * 87 行(0xA1-0xF7) × 94 列(0xA1-0xFE)，每项 uint16 BMP codepoint，\n"
    " * 无效码位 0。查询 O(1)：cp = gbk2312_cp[(hi-0xA1)*94 + (lo-0xA1)]；\n"
    " * GBK 扩展区（繁体/生僻）不在表内，调用方降级 '?'。~16KB rodata。\n"
    " */\n"
    '#include "gbk_table.h"\n\n'
    "const uint16_t gbk2312_cp[%d] = {\n" % len(cells)
    + "".join(
        "    " + ",".join("0x%04X" % cp for cp in cells[i:i + 12]) + ",\n"
        for i in range(0, len(cells), 12)
    )
    + "};\n",
    encoding="utf-8",
)
pathlib.Path(OUT / "gbk_table.h").write_text(
    "/* gbk_table — GB2312→Unicode codepoint 解码矩阵（自动生成，勿手改）。 */\n"
    "#ifndef GBK_TABLE_H\n#define GBK_TABLE_H\n\n#include <stdint.h>\n\n"
    "extern const uint16_t gbk2312_cp[%d];"
    " /* [(hi-0xA1)*94+(lo-0xA1)] → BMP codepoint */\n\n"
    "#endif /* GBK_TABLE_H */\n" % len(cells),
    encoding="utf-8",
)
print(f"生成 {c}：{valid} 有效码位，{len(cells)*2} bytes rodata")
