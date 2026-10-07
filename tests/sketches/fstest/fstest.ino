// The flash path games use for saves: LittleFS (format, write, read back,
// rename, delete) and the EEPROM emulation, printed on the serial port (115200):
//
//   FS step result
//
// Run on the ESPboy (tools/serial_test.py COM3 20) and in the emulator
// (espboy_headless fstest.bin 20): every line must be the same.
#include <Arduino.h>
#include <LittleFS.h>
#include <EEPROM.h>

static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xffffffffu)
{
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c;
}

static uint8_t buf[3000];

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.printf("FS format %d\n", LittleFS.format());
    Serial.printf("FS begin %d\n", LittleFS.begin());

    uint32_t x = 1;
    for (size_t i = 0; i < sizeof buf; i++) { x = x * 1103515245u + 12345u; buf[i] = (uint8_t)(x >> 16); }
    Serial.printf("FS data %08x\n", (unsigned)crc(buf, sizeof buf));

    for (int f = 0; f < 4; f++) {
        char name[16];
        snprintf(name, sizeof name, "/f%d.bin", f);
        File w = LittleFS.open(name, "w");
        size_t n = w.write(buf + f * 100, sizeof buf - f * 300);
        w.close();
        File r = LittleFS.open(name, "r");
        static uint8_t back[3000];
        size_t m = r.read(back, sizeof back);
        r.close();
        Serial.printf("FS file %s wrote %u read %u crc %08x\n", name, (unsigned)n, (unsigned)m, (unsigned)crc(back, m));
    }
    Serial.printf("FS rename %d\n", LittleFS.rename("/f1.bin", "/g1.bin"));
    Serial.printf("FS remove %d\n", LittleFS.remove("/f2.bin"));
    Dir d = LittleFS.openDir("/");
    while (d.next()) Serial.printf("FS dir %s %u\n", d.fileName().c_str(), (unsigned)d.fileSize());
    File a = LittleFS.open("/f0.bin", "a");
    a.print("appended");
    a.close();
    File r = LittleFS.open("/f0.bin", "r");
    Serial.printf("FS append size %u\n", (unsigned)r.size());
    r.seek(r.size() - 8);
    char tail[9] = { 0 };
    r.read((uint8_t *)tail, 8);
    r.close();
    Serial.printf("FS tail %s\n", tail);
    LittleFS.end();
    Serial.printf("FS remount %d\n", LittleFS.begin());
    File again = LittleFS.open("/g1.bin", "r");
    static uint8_t back2[3000];
    size_t m = again.read(back2, sizeof back2);
    again.close();
    Serial.printf("FS after remount %u %08x\n", (unsigned)m, (unsigned)crc(back2, m));

    EEPROM.begin(512);
    for (int i = 0; i < 512; i++) EEPROM.write(i, (uint8_t)(i * 7 + 3));
    Serial.printf("FS eeprom commit %d\n", EEPROM.commit());
    EEPROM.end();
    EEPROM.begin(512);
    uint32_t e = 0;
    for (int i = 0; i < 512; i++) e = e * 31 + EEPROM.read(i);
    Serial.printf("FS eeprom read %08x\n", (unsigned)e);
    Serial.println("FS done");
}

void loop()
{
    delay(1000);
}
