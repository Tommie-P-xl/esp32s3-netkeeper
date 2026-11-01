"""Rebuild a PNG from the firmware's FBDUMP hex dump on the serial console.

The board has no screenshot facility, so ui.c dumps the LVGL snapshot as
"FBDUMP w h" + one hex line per row (RGB565) + "FBDUMP END". This script turns
that back into an image so the layout can actually be looked at.

usage: python fb2png.py <capture.txt> <out.png>
"""

import re
import struct
import sys
import zlib


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


def main():
    src, dst = sys.argv[1], sys.argv[2]
    which = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    text = open(src, "rb").read().decode("utf-8", "replace")

    start = -1
    for _ in range(which + 1):
        start = text.find("FBDUMP ", start + 1)
        if start < 0:
            print("no FBDUMP marker #%d found" % which)
            return 1

    header_end = text.find("\n", start)
    parts = text[start:header_end].split()
    width, height = int(parts[1]), int(parts[2])

    # 每行形如 "R012 <1920 hex>"；按行号放回去，缺行就留黑
    canvas = [[(0, 0, 0)] * width for _ in range(height)]
    got = 0
    for line in text[start:].splitlines():
        line = line.strip()
        if len(line) < 6 or line[0] != "R" or not line[1:4].isdigit():
            continue
        idx = int(line[1:4])
        body = line[5:]
        # 偶尔会有日志插进某一行，把非十六进制字符剔掉再按 4 位切
        body = re.sub(r"[^0-9a-fA-F]", "", body)
        if idx >= height or len(body) < width * 4:
            continue
        canvas[idx] = [rgb565_to_rgb888(int(body[i * 4:i * 4 + 4], 16)) for i in range(width)]
        got += 1

    rows = canvas
    if got != height:
        print("warn: recovered %d/%d rows" % (got, height))

    if len(rows) != height:
        print("warn: got %d/%d rows" % (len(rows), height))

    write_png(dst, width, len(rows), rows)
    print("wrote %s  %dx%d" % (dst, width, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
