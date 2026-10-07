# The ESP8266 ROM

The emulator boots the way the real chip does: through the ESP8266's own 64 KB mask ROM. That
ROM belongs to Espressif and is not included with the emulator; you read it out of any ESP8266
board yourself. It is the same in every ESP8266EX (an ESPboy, a Wemos/LOLIN D1 mini, a NodeMCU,
an ESP-01...), and reading it changes nothing on the board.

Put the file, named `esp8266_rom.bin`, in this `bios` folder next to the emulator. (It is also
found next to the emulator itself, in the current folder, or wherever `--rom` or the
`ESPBOY_ROM` environment variable points. On macOS: in a `bios` folder next to the `.app`.)
For the web version, put it next to `ESPboy_Emulator.html`.

## Reading it with esptool

1. Install esptool: `pip install esptool` (Python 3).
2. Connect the board with USB and find its serial port: `COM3` or similar on Windows (Device
   Manager, "Ports"), `/dev/ttyUSB0` or `/dev/ttyACM0` on Linux, `/dev/cu.usbserial-*` or
   `/dev/cu.wchusbserial*` on macOS. The ESPboy uses a CH340 USB serial chip; Windows and macOS
   may need its driver.
3. Read the ROM:

   ```
   esptool --chip esp8266 --port COM3 --no-stub dump_mem 0x40000000 0x10000 esp8266_rom.bin
   ```

   (older esptool versions are called as `esptool.py`). It takes a few seconds. The result must
   be exactly 65536 bytes; some esptool versions print an error at the very end although the
   file is complete.

With the emulator's source, `python tools/get_rom.py COM3` does the same and puts the file here.
It finds esptool in Python or in the Arduino IDE's esp8266 core.
