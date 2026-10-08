# CLAUDE.md

Working notes for continuing this repository with Claude (or by hand). The README describes the
emulator for users; this file covers how to work on it: setup, verification, design decisions and
the traps already fallen into.

## What this is

An emulator for the **ESPboy**: ESP8266EX (one Xtensa LX106, 80/160 MHz, 64 KB ROM, 64 KB IRAM,
96 KB DRAM, 4 MB flash behind the instruction cache), ST7735S 128x128 on HSPI, MCP23017 (buttons,
display CS, LED lock) and MCP4725 (backlight) on a bit-banged I2C bus, WS2812B LED on GPIO2,
speaker on GPIO0. Board map at the top of `src/esp8266.h`. C11 + SDL3, native and Emscripten
from the same source.

**The owner's priorities (joyrider3774):** speed first, then screen, then sound. Model the
hardware, not a library; the screen follows the ST7735S datasheet, and where the datasheet is
silent, what the ESPboy's panel does (measured). WiFi is stubbed (registers are storage).

**Owner's rule:** never mention other emulator projects in comments or docs (this file included).

## Layout

| Path | What |
|---|---|
| `src/esp8266.h`, `src/esp8266.c` | the machine (`Esp`, `XtCore`), page tables, the flash window, per-page access costs (`esp_update_costs`), decode pages, reset, the run loop (`esp_run`, slices bounded by events and CCOMPARE0) |
| `src/xt_decode.c/.h` | Xtensa decoder + objdump-style disassembler (knows more than the LX106 has) |
| `src/xt_cpu.c` | LX106 executor: CALL0 ABI, XEA2 exceptions, level-1 interrupts + NMI (level 3), the measured cycle model (`read_mask` for the load-use stall) |
| `src/periph.c` | DPORT, WDEV counter + RNG, UART0 (console out, RX FIFO in), SPI1/HSPI (display, deferred capture, TRANS_DONE interrupt), SPI0 (flash commands), GPIO (+ I2C lines, LED decoding, speaker), FRC1/FRC2, RTC, analog I2C storage, PHY wait bits |
| `src/i2c.c` | the bit-banged bus decoded edge by edge; MCP23017 and MCP4725 |
| `src/onewire.c` | a DS18B20 temperature sensor on GPIO2 (1-Wire from the ESP's edges), made-up readings |
| `src/st7735.c/.h` | the panel (datasheet V1.1 + measured ESPboy behaviour) |
| `src/audio.c/.h` | GPIO0 level log integrated per sample, DC blocker |
| `src/loader.c` | ROM dump and flash images |
| `src/main.c` | SDL3 front end (main callbacks; Emscripten too); F6 GIF recording (save dialog natively, showSaveFilePicker or a download in the browser); F7 speaker filter (audio.c, speaker_filter.c: a small speaker's response, 48 kHz FIR, off by default, kept in settings); R or the gamepad's north button turns the screen by quarter turns (the frame buffer is turned after st7735_render, so GIFs and screenshots match; the d-pad is remapped in read_input; kept per program by file name: rotations.txt in the pref folder, localStorage espboy_rotation:<name> on the web; the owner's emulators share the R key for it, F4 is kept for reset to bootloader) |
| `src/gif.c/.h` | GIF encoder: per-frame exact palette (3-3-2 when over 256 colours), identical frames merged, emulated-time delays; `espboy_headless --gif out.gif` |
| `web/shell.html`, `tools/make_web.sh`, `tools/serve.py` | web page, web build, local server |
| `tools/headless.c` | `espboy_headless` |
| `tools/xtdis.c`, `tools/imginfo.py` | disassembler (`--syms` the core's `tools/sdk/ld/eagle.rom.addr.v6.ld` for ROM names); image segments, `--iram` dumps IRAM code by address |
| `tools/build_sketch.py` | Arduino build with the ESPboy board options (`-D` defines, `--flash COM3`) |
| `tools/serial_test.py` | resets the real ESPboy, prints its serial output, types `text@seconds` (same syntax as headless `--serial`) |
| `tools/get_rom.py` | the mask ROM from a device into `bios/esp8266_rom.bin` |
| `tools/build_lge.py` | the Little Game Engine with all its games into `roms/rebuild/LGE/` |
| `tests/sketches/cputest` | instruction checksums (device = emulator, all 75) |
| `tests/sketches/timing` | cycle costs (the source of the cycle model) |
| `tests/sketches/lcdprobe` | panel questions answered by reading frame memory back |
| `tests/sketches/timer1rate` | FRC1 interrupts per second at TIM_DIV1, load 20000 (device = emulator: 3999) |
| `tests/sketches/fstest` | LittleFS + EEPROM |

## Environment (the owner's Windows machine)

- Git Bash and PowerShell; MSYS2 mingw64 gcc, cmake, ninja, SDL3 (static).
- Arduino IDE 1.8 portable in `c:/arduino` (arduino-builder), esp8266 core 3.1.2, LovyanGFX and
  TFT_eSPI in `c:/arduino/portable/sketchbook/libraries`. The lx106 toolchain (objdump,
  addr2line) is on the PATH.
- esptool: the core's copy, run with the system Python and
  `PYTHONPATH="$T/pyserial;$T/esptool"` (`T=c:/arduino/portable/packages/esp8266/hardware/esp8266/3.1.2/tools`).
  The bundled python3.7 ignores PYTHONPATH. esptool's `dump_mem` reports a ValueError at the end
  with Python 3.14 after the file is complete.
- Emscripten 6.0.4 in `c:/github/emsdk`; its Node runs Puppeteer (`puppeteer-core` in the session
  scratchpad) with Chrome for web tests. Port 8123 may be taken by something else: use 8177.
- **A real ESPboy is attached on COM3** (CH340). Its original flash is backed up in
  `device_backup/espboy_flash_backup.bin` (4 MB; restore with `write_flash 0x0`). ROM dump:
  `device_backup/esp8266_rom.bin` = `bios/esp8266_rom.bin`.
- ROMs: `roms/joyrider3774` (the owner's `*_embedded` games, `releases/ESPboy_*.bin`),
  `roms/WiFiAppStore` (the store mirror; fresh copies come from `E:\espboy\WiFiAppStore`),
  `roms/rebuild/LGE`.
- Game sources: `c:/github/*_embedded` (`PlatformESPboy.cpp`, `lib/` = the ESPboy library and
  the LovyanGFX panel setup), `c:/github/bunnymark_ports/espboy` (serial build:
  `-D BUNNYMARK_SERIAL=1`, commands a b u d r m, `fps N bunnies N` lines).

## Build and verify

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/espboy_headless.exe roms/joyrider3774/ESPboy_Blips.bin 10 --screen out.ppm
./build/espboy_headless.exe game.bin 12 --wav out.wav --press a@6 --serial "b@4"
./build/espboy_headless.exe game.bin 5 --trace      # accesses outside known blocks
./build/espboy_headless.exe game.bin 5 --profile    # profile.txt: pc a0 every 1 ms
./build/espboy_headless.exe game.bin 5 --lcdlog     # panel commands
./build/espboy_headless.exe game.bin 5 --i2clog     # MCP23017 / MCP4725 bytes
./build/espboy_headless.exe game.bin 5 --itrace     # last 64 instructions at a fault
./build/espboy_headless.exe game.bin 5 --flashout flash.bin
tools/make_web.sh serve
```

Debug build (slower, not for releases): `cmake -S . -B build_dbg -DCMAKE_C_FLAGS=-DESP_RWATCH`
adds `--rwatch addr len` (loads in a range), `--bp addr` (a function's calls with a2/a3/caller,
stack words, and its returns), `--bp2 addr` (calls only) and `--peek addr` (a word printed at
each `--bp`). malloc/free of a program: find umm_malloc/umm_free in IRAM (`imginfo --iram`).

What the real screen shows: run the program on the ESPboy, then flash `tests/sketches/lcddump`
with esptool `--after no_reset` (the panel keeps its memory), `tools/serial_test.py COM3 15 >
dump.txt`, `tools/lcddump_png.py dump.txt out.png`. The device's RAM after a run: esptool
`--no-stub --after no_reset read_mem addr` (a reset keeps SRAM).

**Device comparison** (the reference for every timing or behaviour question):

```sh
python tools/build_sketch.py tests/sketches/timing --out build/sketches/timing.bin --flash COM3
python tools/serial_test.py COM3 6 | grep TIMING     # device
./build/espboy_headless.exe build/sketches/timing.bin 3 | grep TIMING   # emulator
```

**Regression check after any CPU/peripheral change:**
1. `cputest`: all lines equal to the device's (`diff -w`).
2. `timing`: within a few percent (the jump to a misaligned 24-bit target costs ~0.9 more on the
   chip; not modelled).
3. BunnyMark serial, both libraries: `r@4 d@7 d@10 m@13 m@16 r@19` → device 85/36/23/15/48/83 fps
   (LGFX), 85/36/23/15/43/70 (TFT_eSPI); the emulator within 1-2 fps.
4. Every ROM 15 s with screenshots, contact sheet (the sweep script lives in the session scratchpad:
   headless per ROM, 8 in parallel, PIL sheet). Look at it.

## Design decisions worth keeping

- **Time base**: ticks of 160 MHz. A CPU cycle is 2 ticks at 80 MHz, 1 at 160 (DPORT 0x14 bit 0).
  Peripherals are never ticked: they compute state from `core.cycles` and report events
  (`periph_next_event`); something reprogrammed mid-slice sets `core.stop`.
- **Real boot only**: the 64 KB mask ROM runs, loads eboot from flash, eboot loads the sketch.
  Boot strapping 0xC003 in GPIO_IN[31:16] prints `boot mode:(3,6)` like the device.
- **Flash window**: DPORT 0x3FF0000C bit 25 = flash address bit 20, bits 18:16 = bits 23:21 (the
  ROM's Cache_Read_Enable). Decoded code is cached per physical flash page, so remaps keep it.
- **Cycle model** (measured, `timing`): ALU 1, MULL 2, DRAM load/store 1, a load's result used by
  the next instruction +1, loads from IRAM/ROM/flash and L32R literals there 6, taken branch/jump
  3, RET 4, MEMW after a store 4; peripheral read 16 ticks, write 11 ticks (bus times, in ticks),
  RTC block read 63, DPORT 1, WDEV 4. Flash cache misses are not modelled.
- **HSPI**: a transfer's registers are captured at the next peripheral access after SPI_CMD, not at
  the CMD write: LovyanGFX (`writeDataRepeat`) starts a transfer and then writes SPI_USER1 with
  its length. Writes of USER/USER1 within 32 ticks of the start do not capture. Busy until
  bits × clock; SPI_SLAVE bit 4 TRANS_DONE + bit 9 enable = interrupt 2 (the GameBoy emulator
  streams frames from it).
- **Display CS**: MCP23017 B0, low = selected; an input pin with its pull-up off reads selected
  (Gamebuino META ports init the panel before configuring B0). D/C is GPIO16 (RTC block).
- **ST7735S addresses**: CASET/RASET keep only the low byte (measured with `lcdprobe`).
- **Panel reads**: in 3-wire mode (SPI_USER bit 16, SIO) the ESP reads the panel's SDA on MOSI;
  RDDID (one dummy clock, then 7C 89 F0), RDDST, RDDPM..RDDSDR and RDID1-3 answer. LovyanGFX's
  autodetect (`LGFX_AUTODETECT`, m1cr0lab's ESPboy library) draws nothing unless RDDID's first
  byte is 0x7C. RAMRD is not modelled.
- **FRC1 clock**: APB 80 MHz whatever the CPU clock (`timer1rate` on the device at 160 MHz).
  The store's Anarch loads 20000: its samples go out at 4 kHz on the device too.
- **DS18B20** (owner's request: fake data): GPIO2 = D4, the header pin shared with the LED data
  line (programs lock the LED with MCP23017 B1). A reset is a low over 300 us, slots sample at
  15 us, a 0 sent holds the line to 30 us; ROM 28 45 53 50 62 6F 79 + CRC; 20-24 degrees over a
  two-minute triangle (m1cr0lab's Thermometer).
- **Glass position**: MADCTL 0xC8 window column 2 row 3 (TFT_eSPI GREENTAB3, LovyanGFX's setup);
  BGR bit set by both libraries = correct colours.
- **I2C**: open drain: a line is low when the ESP enables the pin with output 0 or a slave pulls.
  `i2c_lines` gets "the ESP lets SDA go" (msda), not the line level.
- **NMI**: DPORT 0x3FF00000 bits 4:0 = 15 routes FRC1 to the NMI (NmiTimSetFunc): tone(), PWM.
- **Backlight**: MCP4725 code 250..1525 → 0..100% (LovyanGFX's maximum is 1525). An estimate.
- **WiFi/PHY stubs**: RTC 0x60000728 bit 0 (wake), 0x6000057C bit 31 (IQ estimate done),
  0x60009B60 bit 1 cleared (measurement done), analog block 0x62 reg 7 bit 7 (RF PLL calibrated,
  libphy `wait_rfpll_cal_end`; without it "pll_cal exceeds 2ms").
- **WiFi off at boot** (owner's decision): a loaded program gets the SDK's system parameter
  sectors (0x3FD000/0x3FE000/0x3FF000, `src/sdk_params.c` from `tools/gen_sdk_params.py`) with
  WiFi mode off, as an ESPboy whose last program turned it off. With blank parameters the SDK
  starts WiFi (softAP: "bcn 0 / del if1 / usl / mode : null" when the program stops it) and its
  buffers fragment the heap: the store's 2021-era ZX Spectrum (LFC) and LGE builds then failed
  their big allocations (malloc(48 KB) = NULL, black screen / menu that ignores keys). Found by
  reading the device's RAM (esptool read_mem after a reset keeps SRAM) and comparing.
- **RNG** at 0x3FF20E44 (Arduino random()): xorshift, same seed each power-on.
- **Saves**: the whole flash goes to `<program>.sav` (web: IndexedDB `/espboy/saves`), per program
  file. Loading restores only the sectors past the program image (`esp_apply_save`), so a newer
  build of a game keeps its code and its EEPROM/LittleFS saves (`tests/sketches/savetest`:
  two games count separately, a 20 KB bigger build of game 1 keeps its count).
  `espboy_headless --save file` does the same.
- **The ROM** is read at start from a file, never embedded in the program and not in the
  repository: `bios/` (the folder for it, with `bios/README.md` on how to read it from any
  board), the current folder, next to the executable, `--rom`, `$ESPBOY_ROM`; in a macOS .app
  also next to the bundle and in a `bios` folder there. The macOS CI builds put `bios/` inside
  the bundle, in `Contents/Resources` (SDL's base path, searched first), before the ad hoc
  codesign, so it travels with the .app (a quarantined app opened where it was unzipped runs
  from a translocated copy, which loses anything next to it). CI builds include it when the repository
  setting `ESP8266_ROM_URL` (secret or variable) is set: `.github/get_rom.sh` downloads it and
  checks the size and SHA-256 (`ESP8266_ROM_SHA256` in build.yml), and the zips get it in `bios/`
  (the web zip next to the page). Without the setting they carry `bios/README.md` only (owner's
  decision, 2026-10-08; it replaced "never in CI artifacts"). `roms/` is for games.

## Traps

- **Edit C sources with the editor tools**, not Python/heredoc replacement: heredoc-embedded
  Python turned `\n` inside C string literals into real newlines several times here.
- Windows Python and Git Bash paths: `/c/github/...` means `C:\c\github` to Python; use `c:/...`.
- `.align` inside inline asm in IRAM fills with zero bytes (illegal instructions) when executed.
- Keep screenshots, wavs, profile.txt and test cards out of the repo; scratch in the scratchpad.
- After flashing the device, its previous program's panel state may linger (the panel is not reset
  with the ESP).

## Open items (as of 2026-10-05)

- 2026-10-05 retest: all 299 ROMs reach their title screen except MicroPython (no display), the
  WiFi tools and the add-on module tools (Sub1GHz, LoRa, FM radio: no module).
- 2026-10-06: `roms/GitHub/<repo>/` holds sketches built from the ESPboy repos on GitHub that
  the store does not have (deduplicated by content and title), `roms/WebAppStore/` what only the
  m1cr0lab web store has. Some Arduboy ports call `boot()` instead of `begin()`, skipping the
  library's 128x64 window: their frames alternate between the screen's halves (a port bug, the
  panel does the same); some start with the library's flashlight screen (white, any key).
- Flash cache misses (32 KB cache) are not modelled: big CPU-bound programs may run a little fast.
- RAMRD (reading frame memory back over SPI) is not modelled.
- Settings: window size is not kept; volume and scaling are.
