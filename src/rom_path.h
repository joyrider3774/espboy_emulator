/*
 * Where the ESP8266 ROM dump is looked for when none is given: $ESPBOY_ROM,
 * then esp8266_rom.bin in the current folder and in bios/ (tools/get_rom.py
 * writes it there).
 */
#ifndef ROM_PATH_H
#define ROM_PATH_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static bool rom_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool find_rom(char *out, size_t n)
{
    const char *env = getenv("ESPBOY_ROM");
    if (env && rom_exists(env)) { snprintf(out, n, "%s", env); return true; }
    static const char *const tries[] = { "esp8266_rom.bin", "bios/esp8266_rom.bin", "../bios/esp8266_rom.bin" };
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++)
        if (rom_exists(tries[i])) { snprintf(out, n, "%s", tries[i]); return true; }
    return false;
}

#endif
