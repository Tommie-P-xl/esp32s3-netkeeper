"""Mask sensitive regions in an FBDUMP capture, then write a PNG.

README example images must not leak real SSIDs / account names. Rather than
editing PNGs afterwards (no PIL on this machine), the mask is applied to the
raw pixel rows while rebuilding the image — a solid dark rounded-ish block is
drawn over the given rectangles.

usage: python mask_dump.py <capture.txt> <out.png> x0 y0 x1 y1 [more rects...]
  coordinates are in 240x240 image space (the dump is already 2x downsampled)
"""

import struct
import sys
import zlib

import re


def write_png(path, width, height, rgb_rows):
    flat = []
    for row in rgb_rows:
        line = bytearray()
        for px in row:
            line.extend(px)
        flat.append(bytes(line))
    raw = b"".join(b"\x00" + row for row in flat)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def rgb565_to_rgb888(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def load_dump(src):
    text = open(src, "rb").read().decode("utf-8", "replace")
    m = re.search(r"FBDUMP (\d+) (\d+)", text)
    if not m:
        raise SystemExit("no FBDUMP header")
    w, h = int(m.group(1)), int(m.group(2))
    rows = []
    short = 0
    for line in text.splitlines():
        if not re.match(r"R\d{3} ", line):
            continue
        hexpart = line[5:].strip()
        # 偶尔会有日志插进某一行，把非十六进制字符剔掉再按 4 位切
        hexpart = re.sub(r"[^0-9a-fA-F]", "", hexpart)
        row = []
        for i in range(0, len(hexpart) - 3, 4):
            row.append(rgb565_to_rgb888(int(hexpart[i:i + 4], 16)))
        # 串口没有流控，某一行可能少几个像素。PNG 要求每行定长，否则解码器
        # 会从这一行开始错位、后面的内容整片变黑（看起来像截图被截断了）。
        # 缺的部分按黑补：丢的总是行尾，那块基本都是背景。
        if len(row) < w:
            short += 1
            row.extend([(0, 0, 0)] * (w - len(row)))
        rows.append(row[:w])
    if short:
        print("warn: padded %d short row(s) with black" % short)
    if len(rows) != h:
        print("warn: got %d/%d rows, missing ones left black" % (len(rows), h))
        rows.extend([[(0, 0, 0)] * w for _ in range(h - len(rows))])
    return w, h, rows[:h]


def mosaic(rows, rect, blk=12, fade=0.55, card=(16, 16, 16)):
    """像素化打码：把区域内每 blk x blk 的方块替换成该块的平均色，再往卡片色淡化。

    比涂死黑块好在三点：
    1) 块色取自原图，不会出现「比卡片更亮的长方形补丁」那种像渲染错误的效果；
    2) 一眼能看出「这里有内容、被故意遮掉了」，是大家熟悉的打码形态；
    3) blk=12 约等于一个 18px 字的字宽，一个字只落一块平均色，笔画无法还原；
       fade 再把这层平均色往卡片色压 55%，残留对比度更低，更不可能反推。
    """
    x0, y0, x1, y1 = rect
    for by in range(y0, y1, blk):
        for bx in range(x0, x1, blk):
            ys = range(by, min(by + blk, y1))
            xs = range(bx, min(bx + blk, x1))
            n = 0
            sr = sg = sb = 0
            for y in ys:
                for x in xs:
                    r, g, b = rows[y][x]
                    sr += r; sg += g; sb += b; n += 1
            if n == 0:
                continue
            avg = tuple(int((s / n) * (1.0 - fade) + c * fade)
                        for s, c in zip((sr, sg, sb), card))
            for y in ys:
                for x in xs:
                    rows[y][x] = avg


def main():
    """usage: mask_dump.py <capture.txt> <out.png> [M|S x0 y0 x1 y1]...

    M = 马赛克（默认，适合 SSID 这类「看一眼就够」的字段）
    S = 纯色块（适合学号/密码这类必须彻底毁掉的字段，用卡片色，不像补丁）
    """
    src, dst = sys.argv[1], sys.argv[2]
    toks = sys.argv[3:]
    plan = []          # [(mode, (x0,y0,x1,y1))]
    i = 0
    pending = "M"
    while i < len(toks):
        t = toks[i]
        if t in ("M", "S"):
            pending = t
            i += 1
            continue
        x0, y0, x1, y1 = (int(v) for v in toks[i:i + 4])
        plan.append((pending, (x0, y0, x1, y1)))
        i += 4

    w, h, rows = load_dump(src)
    for mode, (x0, y0, x1, y1) in plan:
        x0 = max(0, x0); y0 = max(0, y0)
        x1 = min(w, x1); y1 = min(h, y1)
        if mode == "M":
            mosaic(rows, (x0, y0, x1, y1))
            continue
        for y in range(y0, y1):
            for x in range(x0, x1):
                rows[y][x] = (16, 16, 16)  # 卡片底色，遮完看着像「没有这个字段」

    write_png(dst, w, h, rows)
    print("wrote %s %dx%d, %d rect(s)" % (dst, w, h, len(plan)))


if __name__ == "__main__":
    main()
