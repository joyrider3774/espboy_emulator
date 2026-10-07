// Prints what the ESPboy's panel shows, read back from its frame memory, on the
// serial port (115200): "ROW y" and 128 RGB565 pixels in hex per line, then
// "DUMP done". The panel keeps its memory when the ESP8266 is reset or
// flashed, so: run a program on the ESPboy, flash this, and the dump is what
// that program left on the screen (tools/serial_test.py COM3 15 > dump.txt;
// tools/lcddump_png.py dump.txt out.png). It does not reset or clear the panel.
#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "lib/LGFX_ESP8266_ESPboy.hpp"

static LGFX lcd;

void setup()
{
    Serial.begin(115200);
    delay(300);
    lcd.init_without_reset();
    static uint16_t line[128];
    Serial.println("\nDUMP start");
    for (int y = 0; y < 128; y++) {
        lcd.readRect(0, y, 128, 1, line);
        Serial.printf("ROW %d ", y);
        for (int x = 0; x < 128; x++) Serial.printf("%04x", line[x]);
        Serial.println();
    }
    Serial.println("DUMP done");
}

void loop()
{
    delay(1000);
}
