"""Lists the segments of an ESP8266 flash image (eboot at 0, the sketch at 0x1000) and writes
them out by address, for tools/xtdis:

    python tools/imginfo.py game.bin                 the segments
    python tools/imginfo.py game.bin --iram out.bin  IRAM 0x40100000-0x4010FFFF as the app loads it

xtdis out.bin 40100000 401007a0 40100800 then disassembles the code in IRAM.
"""
import struct
import sys


def segments(data, base):
    magic, count = data[base], data[base + 1]
    if magic != 0xE9:
        return None, []
    entry = struct.unpack_from("<I", data, base + 4)[0]
    off = base + 8
    segs = []
    for _ in range(count):
        addr, size = struct.unpack_from("<II", data, off)
        segs.append((addr, off + 8, size))
        off += 8 + size
    return entry, segs


def main():
    data = open(sys.argv[1], "rb").read()
    images = []
    for base in (0, 0x1000):
        if base < len(data) and data[base] == 0xE9:
            entry, segs = segments(data, base)
            images.append((base, entry, segs))
            print("image at 0x%x: entry 0x%08x" % (base, entry))
            for addr, off, size in segs:
                print("  0x%08x  %6d bytes  (file 0x%x)" % (addr, size, off))
    # the sketch image also starts with the irom0 part at 0x40201010 (not a loaded segment)
    if "--iram" in sys.argv:
        out = sys.argv[sys.argv.index("--iram") + 1]
        iram = bytearray(0x10000)
        for base, entry, segs in images:
            for addr, off, size in segs:
                if 0x40100000 <= addr < 0x40110000:
                    a = addr - 0x40100000
                    iram[a:a + size] = data[off:off + size]
        open(out, "wb").write(iram)
        print("wrote", out)


if __name__ == "__main__":
    main()
