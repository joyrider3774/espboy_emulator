"""Resets an ESPboy on a serial port, prints what it writes and types text into it at given times,
the way espboy_headless --serial does in the emulator, so a test sketch's output can be compared.

    python tools/serial_test.py COM3 seconds [text@seconds ...] [--baud 115200]

Times count from the reset. pyserial comes from the esp8266 core's tools.
"""
import os
import sys
import time

TOOLS = "c:/arduino/portable/packages/esp8266/hardware/esp8266/3.1.2/tools"
sys.path.insert(0, os.path.join(TOOLS, "pyserial"))
import serial  # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    baud = 115200
    if "--baud" in sys.argv:
        baud = int(sys.argv[sys.argv.index("--baud") + 1])
        args.remove(str(baud))
    port, seconds = args[0], float(args[1])
    sends = []
    for a in args[2:]:
        text, at = a.rsplit("@", 1)
        sends.append([float(at), text.encode().decode("unicode_escape").encode("latin-1"), False])
    s = serial.Serial(port, baud, timeout=0.05)
    # EN low through RTS with GPIO0 high (DTR off): a normal reset into the program
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    t0 = time.time()
    buf = b""
    while time.time() - t0 < seconds:
        now = time.time() - t0
        for item in sends:
            if not item[2] and now >= item[0]:
                s.write(item[1])
                item[2] = True
        buf += s.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            # the ROM's boot messages come at 74880 baud and show as noise here
            text = line.decode("ascii", "replace").rstrip("\r").encode("ascii", "replace").decode()
            print("%7.3f %s" % (time.time() - t0, text), flush=True)
    s.close()


if __name__ == "__main__":
    main()
