// Saves across runs: reads a counter from EEPROM, prints it, adds one and
// commits, on the serial port (115200) as
//
//   SAVE game N count C
//
// Built as two games (-D GAME=1, -D GAME=2) and run twice each with
// espboy_headless --save <file>, every game counts its own runs. -D PAD=1
// makes the program bigger: a newer build of the same game must still find
// its count.
#include <Arduino.h>
#include <EEPROM.h>

#ifndef GAME
#define GAME 1
#endif
#ifndef PAD
#define PAD 0
#endif

#if PAD
static const uint8_t padding[20000] PROGMEM = { 1 };
#endif

void setup()
{
    Serial.begin(115200);
    delay(200);
    EEPROM.begin(64);
    uint32_t magic, count;
    EEPROM.get(0, magic);
    EEPROM.get(4, count);
    if (magic != 0x5A5A0000u + GAME) count = 0;
    Serial.printf("\nSAVE game %d count %u\n", GAME, (unsigned)count);
    magic = 0x5A5A0000u + GAME;
    count++;
    EEPROM.put(0, magic);
    EEPROM.put(4, count);
    EEPROM.commit();
#if PAD
    Serial.printf("SAVE pad %u\n", (unsigned)pgm_read_byte(&padding[0]));
#endif
}

void loop()
{
    delay(1000);
}
