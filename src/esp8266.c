/*
 * The chip around the core: memory map, the flash window, decoded code
 * pages, reset and the run loop.
 *
 * Peripherals live in periph.c. A register nothing models keeps what was
 * written and reads it back; with esp_trace_io on, accesses outside every
 * known block are logged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp8266.h"

bool esp_trace_io = false;
uint32_t esp_watch = 0xffffffffu;

void esp_watch_hit(Esp *s, uint32_t a, uint32_t v, int size)
{
    fprintf(stderr, "[watch] pc %08x writes %08x (%d bytes) to %08x\n", s->core.pc, v, size, a);
}

#ifdef ESP_RWATCH
uint32_t esp_rwatch = 0xffffffffu, esp_rwatch_len = 0, esp_bp = 0xffffffffu, esp_bp_peek = 0, esp_bp2 = 0xffffffffu;

void esp_rwatch_hit(Esp *s, uint32_t a, int size)
{
    static unsigned n;
    if (n++ < 2000)
        fprintf(stderr, "[rwatch] %.6f pc %08x reads %d bytes at %08x  a0 %08x a2 %08x a3 %08x a4 %08x a5 %08x a6 %08x a12 %08x a13 %08x\n",
                (double)s->core.cycles / ESP_TICK_HZ, s->core.pc, size, a, s->core.ar[0], s->core.ar[2],
                s->core.ar[3], s->core.ar[4], s->core.ar[5], s->core.ar[6], s->core.ar[12], s->core.ar[13]);
}
#endif

/* ------------------------------------------------------------------------ */
/* Memory map                                                                */
/* ------------------------------------------------------------------------ */

static void map(Esp *s, uint32_t base, uint32_t size, uint8_t *host, bool writable, uint8_t kind)
{
    for (uint32_t off = 0; off < size; off += 0x10000) {
        const uint32_t page = (base + off) >> 16;
        s->rpage[page] = host + off;
        s->wpage[page] = writable ? host + off : NULL;
        s->wkind[page] = kind;
        s->dpage[page] = NULL;
    }
}

/* The cache maps a 1 MB block of flash at 0x40200000 (DPORT 0x3FF0000C,
   written by the ROM's Cache_Read_Enable): bit 25 is flash address bit 20,
   bits 18:16 are bits 23:21. The decoded pages are per flash page, so they
   survive a remap */
void esp_map_window(Esp *s)
{
    const uint32_t r = s->dport[0x0C / 4];
    s->window = ((r >> 25) & 1u) << 20 | ((r >> 16) & 7u) << 21;
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t page = (0x40200000u >> 16) + i;
        const uint32_t off = s->window + (i << 16);
        s->rpage[page] = off < ESP_FLASH_SIZE ? s->flash + off : NULL;
        s->wpage[page] = NULL;
        s->wkind[page] = 0;
        s->dpage[page] = NULL;
    }
}

/* What a load or store costs beyond its one cycle, measured on the ESPboy
   with tests/sketches/timing at 160 MHz: DRAM nothing; IRAM, ROM and flash
   (through the cache, also the L32R literals of code there) 6 cycles; a
   peripheral register 16 cycles to read, about 11.5 to write; DPORT 1, the
   WDEV counter 4. Peripheral times are bus times, so they are kept in ticks;
   the instruction memories run at the CPU clock */
static void esp_update_costs(Esp *s)
{
    memset(s->rcost, 0, sizeof s->rcost);
    memset(s->wcost, 0, sizeof s->wcost);
    const uint8_t imem = (uint8_t)(5 * s->core.tick_mult);
    s->rcost[0x4000] = s->rcost[0x4010] = imem;
    for (int i = 0; i < 16; i++) s->rcost[0x4020 + i] = imem;
    s->rcost[0x6000] = 15;
    s->wcost[0x6000] = 10;
    s->rcost[0x3FF2] = 3;
}

static void map_rebuild(Esp *s)
{
    memset(s->rpage, 0, sizeof(s->rpage));
    memset(s->wpage, 0, sizeof(s->wpage));
    memset(s->wkind, 0, sizeof(s->wkind));
    memset(s->dpage, 0, sizeof(s->dpage));
    map(s, 0x40000000u, ESP_ROM_SIZE, s->rom, false, 0);
    map(s, 0x40100000u, ESP_IRAM_SIZE, s->iram, true, 1);
    map(s, ESP_DRAM_BASE, ESP_DRAM_SIZE, s->dram, true, 0);
    esp_map_window(s);
    esp_update_costs(s);
}

/* The decoded form of the 64 KB page pc is in, or NULL when it holds no code */
XtInsn *esp_decode_page(Esp *s, uint32_t pc)
{
    const uint32_t page = pc >> 16;
    XtInsn *d = NULL;
    if (page == 0x4000) d = s->dc_rom;
    else if (page == 0x4010) d = s->dc_iram;
    else if (pc - 0x40200000u < 0x100000u) {
        const uint32_t off = s->window + (pc - 0x40200000u);
        if (off >= ESP_FLASH_SIZE) return NULL;
        const uint32_t fp = off >> 16;
        if (!s->dc_flash[fp]) s->dc_flash[fp] = calloc(0x10000, sizeof(XtInsn));
        d = s->dc_flash[fp];
    }
    if (d) s->dpage[page] = d;
    return d;
}

/* Flash was programmed or erased: code decoded from it is dropped */
void esp_flash_changed(Esp *s, uint32_t off, uint32_t len)
{
    if (!len) return;
    const uint32_t first = off >> 16, last = (off + len - 1) >> 16;
    for (uint32_t p = first; p <= last && p < ESP_FLASH_PAGES; p++)
        if (s->dc_flash[p]) memset(s->dc_flash[p], 0, 0x10000 * sizeof(XtInsn));
    s->flash_dirty = true;
}

void esp_invalidate_iram(Esp *s, uint32_t off, int len)
{
    /* an instruction up to 2 bytes before the store may reach into it */
    const uint32_t first = off >= 2 ? off - 2 : 0;
    for (uint32_t i = first; i < off + (uint32_t)len && i < ESP_IRAM_SIZE; i++)
        s->dc_iram[i].len = 0;
}

/* ------------------------------------------------------------------------ */
/* Creation and reset                                                        */
/* ------------------------------------------------------------------------ */

Esp *esp_create(void)
{
    Esp *s = calloc(1, sizeof(Esp));
    if (!s) return NULL;
    s->rom = calloc(1, ESP_ROM_SIZE);
    s->iram = calloc(1, ESP_IRAM_SIZE);
    s->dram = calloc(1, ESP_DRAM_SIZE);
    s->flash = malloc(ESP_FLASH_SIZE);
    s->io = calloc(1, 0x10000);
    s->wifi = calloc(1, 0x10000);
    s->dc_rom = calloc(ESP_ROM_SIZE, sizeof(XtInsn));
    s->dc_iram = calloc(ESP_IRAM_SIZE, sizeof(XtInsn));
    if (!s->rom || !s->iram || !s->dram || !s->flash || !s->io || !s->wifi || !s->dc_rom || !s->dc_iram) {
        esp_destroy(s);
        return NULL;
    }
    memset(s->flash, 0xff, ESP_FLASH_SIZE);
    esp_reset(s, true);
    return s;
}

void esp_destroy(Esp *s)
{
    if (!s) return;
    free(s->rom); free(s->iram); free(s->dram); free(s->flash); free(s->io); free(s->wifi);
    free(s->dc_rom); free(s->dc_iram);
    for (unsigned i = 0; i < ESP_FLASH_PAGES; i++) free(s->dc_flash[i]);
    free(s);
}

/* power_on: everything from scratch, time from 0. Otherwise a reset of the
   chip (ESP.restart, the watchdog): time goes on, RTC memory keeps its
   contents, and the chips on the board keep their state */
void esp_reset(Esp *s, bool power_on)
{
    if (power_on) {
        /* RAM comes up holding junk */
        uint32_t r = 0x2545F491u;
        for (uint32_t i = 0; i < ESP_DRAM_SIZE; i += 4) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            memcpy(s->dram + i, &r, 4);
        }
        for (uint32_t i = 0; i < ESP_IRAM_SIZE; i += 4) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            memcpy(s->iram + i, &r, 4);
        }
        memset(s->io, 0, 0x10000);
        for (uint32_t i = 0x1000; i < 0x1400; i += 4) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            memcpy(s->io + i, &r, 4);
        }
        s->core.cycles = 0;
        st7735_power_on(&s->lcd);
        s->lcd.rst_level = true;
        i2c_reset(&s->i2c, true);
        s->led_rgb = 0;
        s->spk_level = 0;
        s->spk_head = s->spk_tail = 0;
    } else {
        /* the peripheral registers reset; RTC memory (0x60001000-0x600013FF) does not */
        uint8_t rtcmem[0x400];
        memcpy(rtcmem, s->io + 0x1000, sizeof rtcmem);
        memset(s->io, 0, 0x10000);
        memcpy(s->io + 0x1000, rtcmem, sizeof rtcmem);
    }
    memset(s->dc_iram, 0, ESP_IRAM_SIZE * sizeof(XtInsn));
    memset(s->iram_code, 0, sizeof(s->iram_code));
    memset(s->wifi, 0, 0x10000);
    memset(s->dport, 0, sizeof(s->dport));
    s->cpu_hz = 80000000u;
    s->irq_level = 0;
    xt_reset(&s->core);
    periph_reset(s, power_on);
    map_rebuild(s);
    s->reset_request = 0;
}

/* ------------------------------------------------------------------------ */
/* Peripheral access                                                         */
/* ------------------------------------------------------------------------ */

uint32_t esp_read(Esp *s, uint32_t addr, int size)
{
    /* unaligned or page-crossing accesses to memory are split into bytes */
    if (s->rpage[addr >> 16]) {
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)mem_read(s, addr + (uint32_t)i, 1) << (8 * i);
        return v;
    }
    const uint32_t word = addr & ~3u;
    uint32_t v;
    if ((addr >> 16) == 0x6000 || (addr >> 8) == 0x3FF000 || (addr >> 16) == 0x3FF2)
        v = periph_read(s, word);
    else {
        if (esp_trace_io)
            fprintf(stderr, "[%08x] read of unmapped %08x\n", s->core.pc, addr);
        return 0;
    }
    v >>= (addr & 3) * 8;
    return size == 4 ? v : size == 2 ? (v & 0xffff) : (v & 0xff);
}

void esp_write(Esp *s, uint32_t addr, uint32_t v, int size)
{
    if (s->wpage[addr >> 16]) {
        for (int i = 0; i < size; i++) mem_write(s, addr + (uint32_t)i, v >> (8 * i), 1);
        return;
    }
    if ((addr >> 16) == 0x6000 || (addr >> 8) == 0x3FF000 || (addr >> 16) == 0x3FF2) {
        const uint32_t word = addr & ~3u;
        if (size != 4) {
            /* narrow writes merge into the register */
            const uint32_t sh = (addr & 3) * 8;
            const uint32_t mask = (size == 2 ? 0xffffu : 0xffu) << sh;
            const uint32_t cur = periph_read(s, word);
            v = (cur & ~mask) | ((v << sh) & mask);
        }
        periph_write(s, word, v);
        return;
    }
    if (esp_trace_io)
        fprintf(stderr, "[%08x] write %08x to unmapped %08x\n", s->core.pc, v, addr);
}

/* ------------------------------------------------------------------------ */
/* Running                                                                   */
/* ------------------------------------------------------------------------ */

#define SLICE 8000u     /* ticks between looks at the peripherals, 50 us */

/* The CPU clock the software selected (DPORT CPU2X) */
void esp_set_cpu_hz(Esp *s, uint32_t hz)
{
    if (!hz || hz == s->cpu_hz) return;
    s->cpu_hz = hz;
    xt_set_tick_mult(&s->core, ESP_TICK_HZ / hz);
    esp_update_costs(s);
}

/* the tick CCOUNT reaches CCOMPARE0 next */
static uint64_t ccompare_tick(const XtCore *c)
{
    const uint32_t delta = c->ccompare - xt_ccount(c);
    return c->cycles + (uint64_t)(delta ? delta : 0x100000000ull) * c->tick_mult;
}

void esp_run(Esp *s, uint64_t ticks)
{
    XtCore *c = &s->core;
    const uint64_t end = c->cycles + ticks;
    while (c->cycles < end) {
        uint64_t slice = c->cycles + SLICE;
        if (slice > end) slice = end;
        const uint64_t ev = periph_next_event(s);
        if (ev > c->cycles && ev < slice) slice = ev;
        /* the timer compare: the slice ends on the cycle CCOUNT gets there */
        const uint64_t cc = ccompare_tick(c);
        if (cc < slice) slice = cc;
        const uint32_t before = xt_ccount(c);
        xt_run(s, c, slice);
        const uint32_t after = xt_ccount(c);
        if ((uint32_t)(c->ccompare - before - 1) < (uint32_t)(after - before)) {
            c->interrupt |= 1u << INUM_TIMER0;
            xt_irq_changed(s);
        }
        periph_events(s);
        /* a reset asked for during the slice happens between slices */
        if (s->reset_request) esp_reset(s, false);
    }
}

/* ------------------------------------------------------------------------ */
/* Console                                                                   */
/* ------------------------------------------------------------------------ */

void esp_console_byte(Esp *s, uint8_t b)
{
    if (b == '\r') return;
    if (b == '\n' || s->con_len >= (int)sizeof(s->con_line) - 1) {
        s->con_line[s->con_len] = 0;
        if (s->console) s->console(s->console_ctx, s->con_line);
        else { fputs(s->con_line, stdout); fputc('\n', stdout); fflush(stdout); }
        s->con_len = 0;
        if (b == '\n') return;
    }
    s->con_line[s->con_len++] = (char)b;
}
