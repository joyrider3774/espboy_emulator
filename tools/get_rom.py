"""Reads the 64 KB mask ROM out of an ESP8266 (an ESPboy, a D1 mini, any board) into
bios/esp8266_rom.bin, where the emulator looks for it:

    python tools/get_rom.py COM3            (Windows; /dev/ttyUSB0 and the like elsewhere)

It uses esptool (pip install esptool, or the copy the Arduino esp8266 core ships in
<arduino>/packages/esp8266/hardware/esp8266/<version>/tools). The board is put in its
download mode by esptool through DTR/RTS, as for flashing; nothing on it is changed.
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def esptool_command():
    try:
        import esptool  # noqa: F401
        return [sys.executable, "-m", "esptool"], None
    except ImportError:
        pass
    roots = [os.path.join("c:/arduino/portable/packages"),
             os.path.join(os.path.expanduser("~"), "AppData", "Local", "Arduino15", "packages"),
             os.path.join(os.path.expanduser("~"), ".arduino15", "packages"),
             os.path.join(os.path.expanduser("~"), "Library", "Arduino15", "packages")]
    for root in roots:
        found = sorted(glob.glob(os.path.join(root, "esp8266", "hardware", "esp8266", "*", "tools", "esptool", "esptool.py")))
        if found:
            tools = os.path.dirname(os.path.dirname(found[-1]))
            env = dict(os.environ, PYTHONPATH=os.pathsep.join([os.path.join(tools, "pyserial"), os.path.join(tools, "esptool")]))
            return [sys.executable, found[-1]], env
    sys.exit("esptool not found: pip install esptool")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    out = os.path.join(HERE, "bios", "esp8266_rom.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd, env = esptool_command()
    subprocess.run(cmd + ["--chip", "esp8266", "--port", port, "--no-stub", "dump_mem", "0x40000000", "0x10000", out],
                   env=env)
    # some esptool versions report an error after the file is complete: judge by the file
    if os.path.isfile(out) and os.path.getsize(out) == 0x10000:
        print("wrote", out)
    else:
        sys.exit("reading the ROM failed")


if __name__ == "__main__":
    main()
