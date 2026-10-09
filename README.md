# ESPboy Emulator

An emulator for the [ESPboy](https://www.espboy.com/), the ESP8266 handheld by RomanS:
an ESP8266EX (Xtensa LX106 at 80/160 MHz), a 1.44" 128x128 ST7735S display, 8 buttons on an
MCP23017 expander, an MCP4725 DAC for the backlight, a WS2812B RGB LED and a speaker.

It emulates the hardware, not a game library: the chip's own mask ROM boots the flash image
(eboot, then the Arduino sketch), and the programs drive the display, buttons, LED and
speaker through the same registers and pins as on the device. Anything built for the ESPboy
(the WiFi App Store's games, emulators and tools, Arduino sketches with TFT_eSPI or
LovyanGFX) runs as a `.bin` flash image.

It runs natively (Windows, Linux, macOS) and in the browser (Emscripten), from the same
C11 + SDL3 source.

## Accuracy

Checked against a real ESPboy with the test sketches in `tests/sketches`:

- **CPU**: every LX106 instruction gives the same results as the chip (`cputest`: 75
  checksums over thousands of operands each, all equal), and the cycle model is the chip's
  measured one (`timing`): 1 cycle for most instructions, 2 for MULL, 6 for loads from
  IRAM/flash/ROM and for L32R literals there, the load-use stall, 3 for taken branches,
  16 cycles for a peripheral register read and about 11.5 for a write.
- **Speed**: BunnyMark (LovyanGFX and TFT_eSPI builds) gives the device's frame rate within
  1-2 fps from 0 to 2000 bunnies, at 16, 8 and 1 bpp.
- **Display**: the ST7735S datasheet, plus what the ESPboy's panel does where the datasheet
  says nothing (`lcdprobe` reads the frame memory back: column and row addresses use only
  their low byte).
- **Flash**: LittleFS and EEPROM saves behave as on the device (`fstest`).

WiFi is not emulated: programs that start it run, but no network appears.

A DS18B20 temperature sensor sits on the expansion pin D4 (GPIO2), reporting a made-up room
temperature, so thermometer programs have something to show.

## Using it

```
ESPboy_Emulator game.bin [options]
```

or drop a `.bin` on the window, or press F3.

### Command line

| Option | What it does |
|---|---|
| `--rom esp8266_rom.bin` | the ESP8266's mask ROM (default: `bios/esp8266_rom.bin` or `esp8266_rom.bin` next to the emulator or in the current folder, or `$ESPBOY_ROM`; see below) |
| `--speaker` | the small speaker's sound, for this run |
| `--no-speaker` | the bare pin signal, for this run (F7 switches between the two and remembers the choice) |
| `--integer-scale` | whole multiples of the screen only, for this run |
| `--no-integer-scale` | fill the window, for this run (F8 switches between the two and remembers the choice) |
| `--scale N` | the window's first size, N times the 128x128 screen (1-10, default 4) |
| `--help`, `-h` | list the options and exit (in a message box on Windows) |

The ESP8266 mask ROM is needed and is not included (it is Espressif's): read it out of any
ESP8266 board (an ESPboy, a D1 mini, a NodeMCU) with esptool, as
[bios/README.md](bios/README.md) explains, and put `esp8266_rom.bin` in the `bios` folder next
to the emulator (on macOS next to the `.app`). It is read when the emulator starts; it is
also found next to the emulator, in the current folder, or via `--rom` / `$ESPBOY_ROM`.

On macOS the release builds keep the `bios` folder inside the app bundle
(`ESPboy_Emulator.app/Contents/Resources/bios`), so the ROM goes wherever the app goes. A
`bios` folder next to the `.app` works too.

| Key | ESPboy |
|---|---|
| Arrows | d-pad |
| X, Space / Z | ACT (A) / ESC (B) |
| A / S | left / right side buttons |
| F1 | help |
| F2 | reset |
| F3 | open a program |
| R, gamepad north | turn the screen 90 degrees clockwise; the d-pad turns with it (kept per game) |
| P / hold Tab | pause / fast forward |
| + / - | volume |
| F6 | record a GIF; press again to stop and choose where to save it |
| F7 | sound through the speaker filter / the bare pin signal (off by default) |
| F8 | scaling: fill the window / whole multiples |
| F9 | stats (game fps, speed, backlight, LED) |
| F10 | screenshot |
| F11, Alt+Enter | fullscreen |

A gamepad works too (d-pad or left stick, south = ACT, east = ESC, shoulders = side buttons, north = turn the screen).
The RGB LED shows at the bottom left.

What a program writes to flash (EEPROM, LittleFS, SPIFFS saves) is kept in
`<program>.bin.sav` next to the program, and in the browser's storage on the web version.

## The web version

`tools/make_web.sh` builds it into `build_web/` (Emscripten in `c:/github/emsdk`, or
`$EMSDK`), with your ROM (`bios/esp8266_rom.bin`, when there is one) and every `.bin` under
`roms/` listed in the page's Games menu; `tools/make_web.sh serve` serves it on http://127.0.0.1:8000. Programs
also load from a file, a URL, or `?rom=name.bin`.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

SDL3 is found as an installed package; `-DUSE_VENDORED_SDL=ON` downloads and links it
statically instead. `.github/workflows/build.yml` builds every platform and the web version;
the builds carry `bios/README.md` instead of the ROM.

Also built: `espboy_headless`, which runs a program with no window (console on stdout,
`--screen out.ppm`, `--wav out.wav`, `--press key@seconds`, `--serial text@seconds`, traces),
and `xtdis`, a disassembler.

## License

MIT, see LICENSE. The ESP8266 ROM is Espressif's and is not part of this project.
