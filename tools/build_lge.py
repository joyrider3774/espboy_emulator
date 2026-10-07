"""Rebuilds the ESPboy Little Game Engine from source into roms/rebuild/LGE/LGE_all_games.bin:

    python tools/build_lge.py [--src folder]

The source is https://github.com/ESPboy-edu/ESPboy_little_game_engine (cloned into the temp
folder when --src is not given). The engine bundles all its games (lgegameslist.h) and copies
them into LittleFS on the first start, then lists them. The WiFi App Store's own LGE files are
single-game builds from 2021; this is the current engine.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "https://github.com/ESPboy-edu/ESPboy_little_game_engine"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src")
    args = ap.parse_args()
    src = args.src
    if not src:
        src = os.path.join(tempfile.gettempdir(), "lge_src")
        if not os.path.isdir(src):
            subprocess.run(["git", "clone", "-q", "--depth", "1", REPO, src], check=True)
    # the sketch folder must carry the sketch's name
    sketch = os.path.join(tempfile.gettempdir(), "lge_build", "ESPboy_little_game_engine")
    shutil.rmtree(os.path.dirname(sketch), ignore_errors=True)
    shutil.copytree(src, sketch, ignore=shutil.ignore_patterns(".git"))
    out = os.path.join(HERE, "roms", "rebuild", "LGE", "LGE_all_games.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run([sys.executable, os.path.join(HERE, "tools", "build_sketch.py"), sketch, "--out", out], check=True)
    elf = os.path.splitext(out)[0] + ".elf"
    if os.path.exists(elf):
        os.remove(elf)


if __name__ == "__main__":
    main()
