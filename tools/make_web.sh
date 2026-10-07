#!/bin/sh
# Builds the web version into build_web/ with Emscripten, and puts beside it
#   esp8266_rom.bin    the chip's ROM ($ESPBOY_ROM, else bios/esp8266_rom.bin)
#   roms/...           the programs under roms/ (the owner's games and the
#                      WiFi App Store mirror)
#   games.json         the list the page's Games menu shows
# and optionally serves it:
#
#   tools/make_web.sh            build only
#   tools/make_web.sh serve      build, then serve on http://127.0.0.1:8000
#
# EMSDK defaults to c:/github/emsdk.

EMSDK=${EMSDK:-/c/github/emsdk}
HERE=$(cd "$(dirname "$0")/.." && pwd)
EM="$EMSDK/upstream/emscripten"
PY=$(ls "$EMSDK"/python/*/python.exe 2>/dev/null | head -1)
[ -n "$PY" ] || PY=python3
NODE_BIN=$(dirname "$(ls "$EMSDK"/node/*/bin/node* 2>/dev/null | head -1)")
export EMSDK PATH="$EM:$NODE_BIN:$PATH"

cd "$HERE" || exit 1
# emcmake from the emsdk folder, or the one on the PATH (the GitHub Action's
# setup-emsdk puts it there)
if [ -f "$EM/emcmake.py" ]; then
    EMCMAKE="$PY $EM/emcmake.py"
else
    EMCMAKE=emcmake
fi
GEN=
command -v ninja > /dev/null && GEN="-G Ninja"
$EMCMAKE cmake -S . -B build_web $GEN -DCMAKE_BUILD_TYPE=Release > /dev/null || exit 1
# the shell page is a link option, which cmake does not track: relink when it changes
[ web/shell.html -nt build_web/ESPboy_Emulator.html ] && rm -f build_web/ESPboy_Emulator.html
cmake --build build_web --target ESPboy_Emulator || exit 1

# every build gets its own .js/.wasm URLs (?v= the .wasm's hash), so neither a
# browser nor a CDN can pair an old .js with a new .wasm
"$PY" - "build_web/ESPboy_Emulator.html" "build_web/ESPboy_Emulator.wasm" <<'PY'
import hashlib, re, sys
html, wasm = sys.argv[1], sys.argv[2]
build = hashlib.sha1(open(wasm, "rb").read()).hexdigest()[:12]
t = open(html, encoding="utf-8").read()
t = re.sub(r"""BUILD_ID\s*=\s*["'][^"']*["']""", 'BUILD_ID="%s"' % build, t)
t = re.sub(r'src="?ESPboy_Emulator\.js(\?v=\w+)?"?>', 'src="ESPboy_Emulator.js?v=%s">' % build, t)
# newline="\n": Python on Windows would otherwise write CRLF line ends
open(html, "w", encoding="utf-8", newline="\n").write(t)
print("build", build)
PY

ROM=${ESPBOY_ROM:-bios/esp8266_rom.bin}
if [ -f "$ROM" ]; then
    cp "$ROM" build_web/esp8266_rom.bin
else
    echo "no ESP8266 ROM: run tools/get_rom.py with an ESP8266 attached, or set ESPBOY_ROM"
fi

"$PY" - <<'PY'
import json, os, shutil
games = []
src = "roms"
dst = os.path.join("build_web", "roms")
shutil.rmtree(dst, ignore_errors=True)
for dirpath, dirs, files in os.walk(src):
    dirs.sort()
    for f in sorted(files):
        if not f.lower().endswith(".bin"):
            continue
        rel = os.path.relpath(os.path.join(dirpath, f), src).replace("\\", "/")
        os.makedirs(os.path.join(dst, os.path.dirname(rel)), exist_ok=True)
        shutil.copy(os.path.join(src, rel), os.path.join(dst, rel))
        parts = rel.split("/")
        group = " / ".join(p for p in parts[:-1] if p != "WiFiAppStore") or "Games"
        games.append({"file": "roms/" + rel, "name": os.path.splitext(parts[-1])[0], "group": group})
json.dump(games, open(os.path.join("build_web", "games.json"), "w"), indent=1)
print("%d games in build_web/games.json" % len(games))
PY

if [ "$1" = "serve" ]; then
    "$PY" tools/serve.py 8000 build_web
fi
