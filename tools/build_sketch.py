"""Builds an Arduino sketch for the ESPboy into a flash image (.bin for offset 0).

    python tools/build_sketch.py path/to/Sketch [-D NAME=VALUE ...] [--out file.bin] [--flash COM3]

Uses the Arduino IDE 1.8 folder (c:/arduino, --arduino) and its arduino-builder with the
esp8266 core 3.1.2 and the board options ESPboy games are released with: LOLIN(WEMOS) D1 mini,
160 MHz, 4 MB flash (2 MB filesystem). --flash writes the result to an ESPboy on that serial port
with the core's esptool.

Defines go to the compiler (compiler.c/cpp.extra_flags); the sketch is not changed.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

FQBN = ("esp8266:esp8266:d1_mini:xtal=160,vt=flash,exception=disabled,stacksmash=disabled,ssl=basic,"
        "mmu=3232,non32xfer=fast,eesz=4M2M,ip=lm2f,dbg=Disabled,lvl=None____,wipe=none,baud=921600")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sketch")
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("--out")
    ap.add_argument("--arduino", default="c:/arduino")
    ap.add_argument("--fqbn", default=FQBN)
    ap.add_argument("--flash", metavar="PORT")
    ap.add_argument("--libraries", action="append", default=[], metavar="DIR",
                    help="a libraries folder to use instead of the sketchbook's (repeatable)")
    args = ap.parse_args()

    sketch = os.path.abspath(args.sketch)
    name = os.path.basename(sketch.rstrip("/\\"))
    ino = os.path.join(sketch, name + ".ino")
    if not os.path.isfile(ino):
        sys.exit("no %s" % ino)
    flags = " ".join("-D" + d for d in args.defines)
    portable = os.path.join(args.arduino, "portable")
    libraries = [os.path.abspath(d) for d in args.libraries] or [os.path.join(portable, "sketchbook", "libraries")]
    # a build folder per sketch, defines and libraries, so builds of same-named sketches do not mix
    key = hashlib.sha1("\n".join([sketch, flags] + libraries).encode()).hexdigest()[:10]
    build = os.path.join(tempfile.gettempdir(), "espboy_sketch_build", name + "_" + key)
    os.makedirs(build, exist_ok=True)
    os.makedirs(build + "_cache", exist_ok=True)
    command = [
        os.path.join(args.arduino, "arduino-builder.exe" if os.name == "nt" else "arduino-builder"),
        "-compile", "-logger=human",
        "-hardware", os.path.join(args.arduino, "hardware"),
        "-hardware", os.path.join(portable, "packages"),
        "-tools", os.path.join(args.arduino, "tools-builder"),
        "-tools", os.path.join(args.arduino, "hardware", "tools", "avr"),
        "-tools", os.path.join(portable, "packages"),
        "-built-in-libraries", os.path.join(args.arduino, "libraries"),
    ]
    for lib in libraries:
        command += ["-libraries", lib]
    command += [
        "-fqbn", args.fqbn, "-ide-version=10819",
        "-build-path", build, "-build-cache", build + "_cache",
        "-prefs", "compiler.c.extra_flags=" + flags,
        "-prefs", "compiler.cpp.extra_flags=" + flags,
        ino,
    ]
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        sys.stdout.write(result.stdout[-4000:])
        sys.stderr.write(result.stderr[-4000:])
        sys.exit("build failed")
    out = args.out or os.path.join("build", "sketches", name + ".bin")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    shutil.copy(os.path.join(build, name + ".ino.bin"), out)
    shutil.copy(os.path.join(build, name + ".ino.elf"), os.path.splitext(out)[0] + ".elf")
    print(out)

    if args.flash:
        tools = os.path.join(portable, "packages", "esp8266", "hardware", "esp8266", "3.1.2", "tools")
        env = dict(os.environ, PYTHONPATH=os.pathsep.join([os.path.join(tools, "pyserial"), os.path.join(tools, "esptool")]))
        subprocess.run([sys.executable, os.path.join(tools, "esptool", "esptool.py"), "--chip", "esp8266",
                        "--port", args.flash, "--baud", "460800", "write_flash", "0x0", out], env=env, check=True)


if __name__ == "__main__":
    main()
