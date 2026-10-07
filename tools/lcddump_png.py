"""Turns the serial output of tests/sketches/lcddump (what the ESPboy's panel shows) into a PNG:

    python tools/lcddump_png.py dump.txt out.png

The pixels come back from LovyanGFX as byte-swapped RGB565.
"""
import re
import sys

from PIL import Image


def main():
    img = Image.new("RGB", (128, 128))
    px = img.load()
    rows = 0
    for line in open(sys.argv[1], encoding="ascii", errors="replace"):
        m = re.search(r"ROW (\d+) ([0-9a-f]{512})", line)
        if not m:
            continue
        y = int(m.group(1))
        data = m.group(2)
        for x in range(128):
            v = int(data[x * 4:x * 4 + 4], 16)
            v = ((v & 0xff) << 8) | (v >> 8)
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            px[x, y] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
        rows += 1
    img.save(sys.argv[2])
    print("%d rows -> %s" % (rows, sys.argv[2]))


if __name__ == "__main__":
    main()
