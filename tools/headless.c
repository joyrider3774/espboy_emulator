/*
 * Runs the ESPboy with no window: the console (UART0) goes to stdout.
 *
 *   espboy_headless program.bin [seconds] [--rom esp8266_rom.bin]
 *                   [--screen out.ppm] [--wav out.wav] [--press key@s[:len]]
 *                   [--trace] [--itrace] [--watch addr] [--profile]
 *
 * Keys for --press: left up down right a b lft rgt (the ESPboy's ACT is a,
 * ESC is b, the side buttons lft and rgt).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp8266.h"
#include "audio.h"
#include "gif.h"
#include "rom_path.h"

#ifdef _WIN32
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

static void write_ppm(Esp *s, const char *path)
{
    static uint32_t px[128 * 128];
    st7735_render(&s->lcd, px);
    const float bl = backlight_level(&s->i2c);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n128 128\n255\n");
    for (int i = 0; i < 128 * 128; i++) {
        const uint32_t c = px[i];
        const uint8_t rgb[3] = { (uint8_t)((c >> 16 & 255) * bl), (uint8_t)((c >> 8 & 255) * bl), (uint8_t)((c & 255) * bl) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

typedef struct { uint8_t key; double at, len; } Press;

static uint8_t key_bit(const char *name, size_t n)
{
    static const struct { const char *name; uint8_t bit; } keys[] = {
        { "left", 0x01 }, { "up", 0x02 }, { "down", 0x04 }, { "right", 0x08 },
        { "a", 0x10 }, { "b", 0x20 }, { "lft", 0x40 }, { "rgt", 0x80 },
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (strlen(keys[i].name) == n && !strncmp(keys[i].name, name, n)) return keys[i].bit;
    return 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* the arguments as UTF-8 (the loader's paths), not the ANSI code page */
    {
        int wargc = 0;
        wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv && wargc == argc) {
            for (int i = 0; i < argc; i++) {
                const int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
                char *u = malloc((size_t)len);
                if (!u) break;
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u, len, NULL, NULL);
                argv[i] = u;
            }
        }
    }
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: espboy_headless program.bin [seconds] [--rom path] [--screen out.ppm] [--wav out.wav]\n");
        return 2;
    }
    const char *program = argv[1];
    double seconds = 3;
    char rom[1024] = "";
    /* --save file: the program's saves come from it before the run and go
       back into it after, as the front end does with <program>.sav */
    const char *screen = NULL, *wav_path = NULL, *flashout = NULL, *save_path = NULL;
    const char *gif_path = NULL;    /* --gif out.gif: the screen recorded, as F6 does */
    bool speaker = false;
    bool profile = false;
    Press presses[64];
    int npress = 0;
    struct { const char *text; double at; bool sent; } serial[64];
    int nserial = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) snprintf(rom, sizeof rom, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--trace")) esp_trace_io = true;
        else if (!strcmp(argv[i], "--itrace")) xt_trace = true;
        else if (!strcmp(argv[i], "--lcdlog")) esp_lcd_log = true;        else if (!strcmp(argv[i], "--i2clog")) esp_i2c_log = true;
        else if (!strcmp(argv[i], "--flashout") && i + 1 < argc) flashout = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_path = argv[++i];
        else if (!strcmp(argv[i], "--gif") && i + 1 < argc) gif_path = argv[++i];
        else if (!strcmp(argv[i], "--speaker")) speaker = true;    /* --wav through the speaker filter */
#ifdef ESP_RWATCH
        else if (!strcmp(argv[i], "--rwatch") && i + 2 < argc) {
            esp_rwatch = (uint32_t)strtoul(argv[++i], NULL, 16);
            esp_rwatch_len = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--bp") && i + 1 < argc) {
            extern uint32_t esp_bp;
            esp_bp = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--bp2") && i + 1 < argc) {
            extern uint32_t esp_bp2;
            esp_bp2 = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc) {
            extern uint32_t esp_bp_peek;
            esp_bp_peek = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
#endif
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) screen = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--profile")) profile = true;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) esp_watch = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc && nserial < 64) {
            /* text@seconds: typed into UART0 at that emulated time */
            char *p = argv[++i], *at = strrchr(p, '@');
            if (!at) { fprintf(stderr, "--serial text@seconds\n"); return 2; }
            *at = 0;
            serial[nserial].text = p;
            serial[nserial].at = atof(at + 1);
            serial[nserial].sent = false;
            nserial++;
        }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npress < 64) {
            const char *p = argv[++i], *at = strchr(p, '@');
            if (!at || !(presses[npress].key = key_bit(p, (size_t)(at - p)))) {
                fprintf(stderr, "--press key@seconds[:duration], key one of left up down right a b lft rgt\n");
                return 2;
            }
            presses[npress].at = atof(at + 1);
            const char *colon = strchr(at, ':');
            presses[npress].len = colon ? atof(colon + 1) : 0.2;
            npress++;
        }
        else seconds = atof(argv[i]);
    }
    if (!rom[0] && !find_rom(rom, sizeof rom)) {
        fprintf(stderr, "no ESP8266 ROM found: pass --rom esp8266_rom.bin (tools/get_rom.py reads one from a device)\n");
        return 1;
    }

    Esp *s = esp_create();
    if (!s) { fprintf(stderr, "out of memory\n"); return 1; }
    char err[512];
    if (!esp_load_rom(s, rom, err, sizeof err) || !esp_load_program(s, program, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (save_path) {
        FILE *f = fopen(save_path, "rb");
        if (f) {
            uint8_t *data = malloc(ESP_FLASH_SIZE);
            const size_t n = data ? fread(data, 1, ESP_FLASH_SIZE, f) : 0;
            if (data) esp_apply_save(s, data, n);
            free(data);
            fclose(f);
        }
    }
    esp_reset(s, true);

    EspAudio audio;
    memset(&audio, 0, sizeof audio);
    audio.speaker = speaker;
    esp_audio_init(&audio, 48000, esp_now(s));
    FILE *wav = NULL;
    uint32_t wav_samples = 0;
    if (wav_path) {
        wav = fopen(wav_path, "wb");
        if (wav) { uint8_t hdr[44] = { 0 }; fwrite(hdr, 1, 44, wav); }
    }
    FILE *prof = profile ? fopen("profile.txt", "w") : NULL;
    Gif gif;
    static uint32_t gif_px[128 * 128];
    const bool recording = gif_path && gif_begin(&gif, 128, 128);

    const double t0 = now_s();
    const uint64_t step = ESP_TICK_HZ / 1000;   /* 1 ms */
    const uint64_t total = (uint64_t)(seconds * ESP_TICK_HZ);
    for (uint64_t done = 0; done < total; done += step) {
        const double t = (double)done / ESP_TICK_HZ;
        uint8_t keys = 0;
        for (int i = 0; i < npress; i++)
            if (t >= presses[i].at && t < presses[i].at + presses[i].len) keys |= presses[i].key;
        s->buttons = keys;
        for (int i = 0; i < nserial; i++)
            if (!serial[i].sent && t >= serial[i].at) {
                esp_uart_input(s, (const uint8_t *)serial[i].text, strlen(serial[i].text));
                serial[i].sent = true;
            }
        esp_run(s, step);
        /* the screen every 1/60 s, as the window shows it */
        if (recording && (done / step) % 16 == 0) {
            st7735_render(&s->lcd, gif_px);
            gif_frame(&gif, gif_px, 16.0 / 1000.0);
        }
        if (prof && (done / step) % 1 == 0) fprintf(prof, "%08x %08x\n", s->core.pc, s->core.ar[0]);
        float buf[512];
        int n;
        while ((n = esp_audio_render(&audio, s, buf, 512)) > 0) {
            if (wav) {
                for (int i = 0; i < n; i++) {
                    const int16_t v = (int16_t)(buf[i] * 32000);
                    fwrite(&v, 2, 1, wav);
                }
                wav_samples += (uint32_t)n;
            }
        }
    }
    const double wall = now_s() - t0;
    fprintf(stderr, "%.2f s emulated in %.2f s (%.1fx), pc %08x, %u faults, cpu %u MHz, frames %u, i2c %u, led %06x, backlight %.2f\n",
            seconds, wall, seconds / wall, s->core.pc, s->core.fault_count, s->cpu_hz / 1000000u,
            s->lcd.frame_starts, s->i2c.transactions, s->led_rgb, backlight_level(&s->i2c));
    if (xt_trace && s->core.fault_count) xt_trace_dump();
    if (screen) write_ppm(s, screen);
    if (recording) {
        size_t n = 0;
        uint8_t *data = gif_end(&gif, &n);
        FILE *f = data ? fopen(gif_path, "wb") : NULL;
        if (f) { fwrite(data, 1, n, f); fclose(f); }
        free(data);
    }
    if (save_path && s->flash_dirty) {
        FILE *f = fopen(save_path, "wb");
        if (f) { fwrite(s->flash, 1, ESP_FLASH_SIZE, f); fclose(f); }
    }
    if (flashout) {
        /* the whole flash as the program left it */
        FILE *f = fopen(flashout, "wb");
        if (f) { fwrite(s->flash, 1, ESP_FLASH_SIZE, f); fclose(f); }
    }
    if (wav) {
        uint8_t hdr[44];
        memcpy(hdr, "RIFF", 4); put_le32(hdr + 4, 36 + wav_samples * 2); memcpy(hdr + 8, "WAVEfmt ", 8);
        put_le32(hdr + 16, 16); hdr[20] = 1; hdr[21] = 0; hdr[22] = 1; hdr[23] = 0;
        put_le32(hdr + 24, 48000); put_le32(hdr + 28, 96000); hdr[32] = 2; hdr[33] = 0; hdr[34] = 16; hdr[35] = 0;
        memcpy(hdr + 36, "data", 4); put_le32(hdr + 40, wav_samples * 2);
        fseek(wav, 0, SEEK_SET);
        fwrite(hdr, 1, 44, wav);
        fclose(wav);
    }
    if (prof) fclose(prof);
    esp_destroy(s);
    return 0;
}
