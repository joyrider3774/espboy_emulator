// Questions to the ESPboy's ST7735S that the datasheet leaves open, answered
// by reading the frame memory back (LovyanGFX reads the panel over the
// 3-wire SPI bus) and printed on the serial port (115200):
//
//   PX x y rgb565     every pixel that is not black after the tests
//
// The tests write single pixels with raw CASET/RASET commands, in LovyanGFX's
// own orientation (screen x,y is column x+2, row y+3, see the log of any
// LovyanGFX program):
//   A  column 12, row 13                    red    reference: lands at 10,10
//   B  column 0x010C (high byte set), row 23   green  at 10,20 if the high byte is ignored
//   C  XS 12, XE 0x010C, 3 pixels, row 33   blue   in one column (10,30 10,31 10,32) if XE's
//                                                  high byte is ignored, else 10..12,30
//   D  column 22, row 0x012B (43 + 256)     white  at 20,40 if the high byte is ignored
#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "lib/LGFX_ESP8266_ESPboy.hpp"

static LGFX lcd;

static void window(uint16_t xs, uint16_t xe, uint16_t ys, uint16_t ye)
{
    lcd.writeCommand(0x2A);
    lcd.writeData(xs >> 8); lcd.writeData(xs & 255); lcd.writeData(xe >> 8); lcd.writeData(xe & 255);
    lcd.writeCommand(0x2B);
    lcd.writeData(ys >> 8); lcd.writeData(ys & 255); lcd.writeData(ye >> 8); lcd.writeData(ye & 255);
    lcd.writeCommand(0x2C);
}

static void pixel(uint16_t c)
{
    lcd.writeData(c >> 8);
    lcd.writeData(c & 255);
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    lcd.init();
    lcd.setBrightness(128);
    lcd.fillScreen(TFT_BLACK);

    lcd.startWrite();
    window(12, 12, 13, 13);             pixel(0xF800);                              // A
    window(0x010C, 0x010C, 23, 23);     pixel(0x07E0);                              // B
    window(12, 0x010C, 33, 35);         pixel(0x001F); pixel(0x001F); pixel(0x001F); // C
    window(22, 22, 0x012B, 0x012B);     pixel(0xFFFF);                              // D
    lcd.writeCommand(0x00);
    lcd.endWrite();
    // controls drawn by the library itself: E 1x1 at 5,5 (red), F 3x2 at 50,50 (green)
    lcd.drawPixel(5, 5, TFT_RED);
    lcd.fillRect(50, 50, 3, 2, TFT_GREEN);
    delay(50);

    static uint16_t line[128];
    Serial.println("\nPROBE start");
    for (int y = 0; y < 128; y++) {
        lcd.readRect(0, y, 128, 1, line);
        for (int x = 0; x < 128; x++)
            if (line[x]) Serial.printf("PX %d %d %04x\n", x, y, line[x]);
    }
    Serial.println("PROBE done");
}

void loop()
{
    delay(1000);
}
