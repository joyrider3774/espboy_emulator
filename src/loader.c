/*
 * Loading the mask ROM and programs.
 *
 * The ROM is the chip's 64 KB at 0x40000000, as a raw dump (tools/get_rom.py
 * reads it from an ESP8266 with esptool).
 *
 * A program is what an Arduino build or the WiFi App Store hands out: a flash
 * image for offset 0 (eboot, the core's boot loader, followed by the sketch
 * at 0x1000), starting with the ESP8266 image magic 0xE9. It is written into
 * the 4 MB flash, whose remaining sectors keep what they held (erased the
 * first time): saves and the SDK's settings live there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp8266.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* Paths are UTF-8 (what SDL hands over). Windows' fopen takes them in the
   ANSI code page, which loses names like the App Store's
   "📡_Sub1GHzInspector.bin": there the name goes to _wfopen as UTF-16 */
static FILE *open_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wpath[1024], wmode[8];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) &&
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8))
        return _wfopen(wpath, wmode);
#endif
    return fopen(path, mode);
}

static uint8_t *read_file(const char *path, size_t *n, char *err, size_t errlen)
{
    FILE *f = open_utf8(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = len > 0 ? malloc((size_t)len) : NULL;
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        snprintf(err, errlen, "cannot read %s", path);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return buf;
}

bool esp_load_rom(Esp *s, const char *path, char *err, size_t errlen)
{
    size_t n;
    uint8_t *buf = read_file(path, &n, err, errlen);
    if (!buf) return false;
    if (n != ESP_ROM_SIZE) {
        free(buf);
        snprintf(err, errlen, "%s is %u bytes, not a 64 KB ESP8266 ROM dump", path, (unsigned)n);
        return false;
    }
    memcpy(s->rom, buf, ESP_ROM_SIZE);
    free(buf);
    memset(s->dc_rom, 0, ESP_ROM_SIZE * sizeof(XtInsn));
    return true;
}

bool esp_load_program_mem(Esp *s, const uint8_t *data, size_t n, char *err, size_t errlen)
{
    if (n < 16 || data[0] != 0xE9) {
        snprintf(err, errlen, "not an ESP8266 flash image (no 0xE9 header)");
        return false;
    }
    if (n > ESP_FLASH_SIZE) {
        snprintf(err, errlen, "the image is larger than the 4 MB flash");
        return false;
    }
    memset(s->flash, 0xff, ESP_FLASH_SIZE);
    memcpy(s->flash, data, n);
    /* The SDK's system parameters at the end of the flash say WiFi is off,
       as on an ESPboy whose last program switched it off (nearly every game
       does, and the SDK keeps that setting). With blank parameters the SDK
       would start WiFi and hold its buffers until the program turns it off,
       which leaves the heap too fragmented for some programs (the WiFi App
       Store's ZX Spectrum builds need 48 KB in one piece). A save restores
       the program's own parameters over these (esp_apply_save) */
    if (n <= esp_sdk_params[0].addr)
        for (size_t i = 0; i < esp_sdk_params_count; i++)
            memcpy(s->flash + esp_sdk_params[i].addr, esp_sdk_params[i].data, esp_sdk_params[i].len);
    esp_flash_changed(s, 0, ESP_FLASH_SIZE);
    s->flash_dirty = false;
    s->image_size = (uint32_t)n;
    return true;
}

/* A save is the whole flash as a program left it. Only what lies past the
   program image comes back from it (the sectors after the image: EEPROM,
   LittleFS/SPIFFS, the SDK's settings), so a newer build of the program
   keeps its own code and still finds its saves */
bool esp_apply_save(Esp *s, const uint8_t *data, size_t n)
{
    if (n != ESP_FLASH_SIZE) return false;
    const uint32_t from = (s->image_size + 0xfffu) & ~0xfffu;
    if (from >= ESP_FLASH_SIZE) return false;
    memcpy(s->flash + from, data + from, ESP_FLASH_SIZE - from);
    esp_flash_changed(s, from, ESP_FLASH_SIZE - from);
    s->flash_dirty = false;
    return true;
}

bool esp_load_program(Esp *s, const char *path, char *err, size_t errlen)
{
    size_t n;
    uint8_t *buf = read_file(path, &n, err, errlen);
    if (!buf) return false;
    const bool ok = esp_load_program_mem(s, buf, n, err, errlen);
    free(buf);
    return ok;
}
