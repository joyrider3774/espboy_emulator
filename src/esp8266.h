/*
 * The ESPboy as one machine: an ESP8266EX (one Xtensa LX106 core at 80 or
 * 160 MHz, 64 KB mask ROM, 64 KB IRAM, 96 KB DRAM, 4 MB SPI flash behind the
 * instruction cache) and the board around it:
 *
 *   ST7735S 128x128   HSPI (SCK GPIO14, MOSI GPIO13), D/C GPIO16, CS on MCP23017 B0
 *   MCP23017 (0x20)   I2C, bit-banged by the program on SDA GPIO4 / SCL GPIO5:
 *                     A0-A7 the 8 buttons (low when pressed), B0 TFT CS, B1 LED lock
 *   MCP4725 (0x60)    I2C DAC: the backlight level
 *   WS2812B           the RGB LED, GPIO2 (D4), bit-banged against CCOUNT
 *   speaker           GPIO0 (D3)
 *
 * Address map:
 *   0x3FF00000  DPORT (interrupt edge enables, cache control, CPU clock, eFuse)
 *   0x3FF20000  WiFi MAC/baseband (WiFi is not emulated: plain storage), the
 *               1 MHz WDEV counter at 0x3FF20C00
 *   0x3FFE8000  DRAM, 96 KB (the ROM's data and stack sit at the top)
 *   0x40000000  ROM, 64 KB
 *   0x40100000  IRAM, 32 KB + 32 KB that is the instruction cache when it is on
 *   0x40200000  flash through the cache, a 1 MB window (DPORT 0x3FF0000C)
 *   0x60000000  peripherals; 0x60001000 RTC memory (1 KB, kept over a reset)
 *
 * Memory is reached through 64 KB page tables: a page that is plain memory
 * has a host pointer and loads and stores stay in the CPU's fast path; a NULL
 * page goes to esp_read/esp_write, which model the peripherals.
 */
#ifndef ESP8266_H
#define ESP8266_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "xt_decode.h"
#include "st7735.h"

/* Emulated time is counted in ticks of 160 MHz: a CPU cycle is 2 ticks at
   80 MHz and 1 at 160 MHz; the 80 MHz APB clock (timers, UART, SPI) is 2
   ticks and the WDEV microsecond counter 160 */
#define ESP_TICK_HZ      160000000u
#define ESP_ROM_SIZE     0x10000u
#define ESP_IRAM_SIZE    0x10000u
#define ESP_DRAM_BASE    0x3FFE0000u     /* the host buffer covers both 64 KB pages; DRAM proper starts at 0x3FFE8000 */
#define ESP_DRAM_SIZE    0x20000u
#define ESP_FLASH_SIZE   (4u << 20)
#define ESP_FLASH_PAGES  (ESP_FLASH_SIZE >> 16)
#define ESP_SPK_LOG      65536u          /* speaker level changes not yet turned into sound */

/* ------------------------------------------------------------------------ */
/* CPU core                                                                  */
/* ------------------------------------------------------------------------ */

#define PS_INTLEVEL  0x0000000fu
#define PS_EXCM      0x00000010u
#define PS_UM        0x00000020u

/* interrupt lines (ets_sys.h and the SDK) */
enum {
    INUM_WDEV = 0, INUM_SLC = 1, INUM_SPI = 2, INUM_RTC = 3, INUM_GPIO = 4, INUM_UART = 5,
    INUM_TIMER0 = 6, INUM_SOFT = 7, INUM_WDT = 8, INUM_FRC1 = 9, INUM_FRC2 = 10, INUM_NMI = 14,
};

typedef struct XtCore {
    uint32_t ar[16];
    uint32_t pc;
    uint32_t ps;
    uint32_t sar, litbase;
    uint32_t epc[4], eps[4], excsave[4];    /* [1] .. [3] */
    uint32_t depc, exccause, excvaddr, debugcause;
    uint32_t intenable;
    uint32_t interrupt;         /* INTERRUPT: latched (timer, software, edge, NMI) | level lines */
    uint32_t vecbase, prid, ccompare;
    uint32_t ibreakenable, ibreaka, dbreaka, dbreakc, icount, icountlevel;

    uint64_t cycles;            /* the core's clock in ticks (ESP_TICK_HZ): "now" for everything */
    uint32_t tick_mult;         /* ticks a CPU cycle */
    uint32_t ccount_base;       /* CCOUNT = ccount_base + (cycles - ccount_ticks) / tick_mult */
    uint64_t ccount_ticks;
    uint8_t  loaded;            /* register the last instruction loaded (16: none), for the interlock */
    bool     after_store;       /* the last instruction stored (MEMW then waits) */
    bool     waiting;           /* WAITI until an interrupt */
    bool     in_nmi;            /* the NMI handler runs (until RFI 3) */
    bool     stop;              /* leave the run loop after this instruction */
    uint32_t fault_count;
} XtCore;

/* ------------------------------------------------------------------------ */
/* Board devices                                                             */
/* ------------------------------------------------------------------------ */

/* the bit-banged I2C bus and the two chips on it */
typedef struct {
    bool sda, scl;              /* the line levels */
    bool msda;                  /* the ESP lets SDA go */
    bool slave_low;             /* a slave pulls SDA low */
    int  state;
    int  bit;
    uint8_t byte;
    bool read, master_ack;
    int  dev;                   /* addressed device, -1 none */
    /* MCP23017 */
    uint8_t mcp[0x16];          /* register file, IOCON.BANK=0 layout */
    uint8_t mcp_ptr;
    bool    mcp_first;          /* the next byte written is the register address */
    /* MCP4725 */
    uint16_t dac, dac_eeprom;
    uint8_t  dac_pd, dac_eeprom_pd;
    uint8_t  dac_buf[3];
    int      dac_n, dac_rd;
    uint32_t transactions;
} I2cBus;

/* A DS18B20 temperature sensor on GPIO2 (onewire.c) */
typedef struct {
    int      state;
    uint64_t fall;              /* tick the ESP last pulled the line low */
    bool     low;               /* the ESP pulls the line low */
    uint64_t presence_from, presence_to;
    uint8_t  rx, rx_n;          /* bits of the byte being received, LSB first */
    uint8_t  tx[9];             /* bytes being sent, LSB first */
    int      tx_len, tx_pos;
    bool     tx_bit;            /* the bit of the slot in progress */
    int      search_bit, search_phase;
    uint8_t  cmd;
    uint8_t  th, tl, config;    /* scratchpad bytes 2-4 */
    uint8_t  e_th, e_tl, e_config;  /* their EEPROM copy */
    uint64_t convert_done;      /* tick a temperature conversion ends */
    int16_t  temp;              /* the last conversion, 1/16 degrees */
} OneWire;

/* the speaker pin's level since 'tick' (0..1: sigma-delta gives fractions) */
typedef struct { uint64_t tick; float level; } SpkEvent;

/* ------------------------------------------------------------------------ */
/* The machine                                                               */
/* ------------------------------------------------------------------------ */

typedef struct Esp Esp;

struct Esp {
    XtCore core;

    uint8_t *rom;               /* 64 KB */
    uint8_t *iram;              /* 64 KB */
    uint8_t *dram;              /* 128 KB from 0x3FFE0000 */
    uint8_t *flash;             /* 4 MB */
    uint8_t *io;                /* 0x60000000-0x6000FFFF register storage, RTC memory included */
    uint8_t *wifi;              /* 0x3FF20000-0x3FF2FFFF */
    uint32_t dport[64];         /* 0x3FF00000-0x3FF000FF */

    /* 64 KB page tables */
    uint8_t *rpage[65536];
    uint8_t *wpage[65536];
    uint8_t  wkind[65536];      /* 1: IRAM, stores drop decoded code */
    /* ticks a load / store to a page adds to its one cycle (esp_update_costs) */
    uint8_t  rcost[65536];
    uint8_t  wcost[65536];

    /* decoded instructions, one per byte address of code */
    XtInsn  *dpage[65536];
    XtInsn  *dc_rom;
    XtInsn  *dc_iram;
    XtInsn  *dc_flash[ESP_FLASH_PAGES];  /* by physical flash page, so a remap keeps them */
    uint8_t  iram_code[ESP_IRAM_SIZE >> 6];
    uint32_t window;            /* flash offset at 0x40200000 */
    bool     flash_dirty;       /* the program wrote flash since the last save */
    uint32_t image_size;        /* bytes of the loaded program image (from flash offset 0) */
    uint8_t  flash_status;      /* the flash chip's status register (WEL) */

    /* console: UART0 output, a line at a time */
    char     con_line[512];
    int      con_len;
    void   (*console)(void *ctx, const char *line);
    void    *console_ctx;
    /* UART0 input: bytes waiting in the RX FIFO (esp_uart_input) */
    uint8_t  uart_rx[256];
    uint8_t  uart_rx_head, uart_rx_tail;
    uint32_t rng;               /* the hardware RNG (0x3FF20E44) */

    uint32_t cpu_hz;
    uint32_t reset_request;     /* a software reset asked for, done between slices */

    /* interrupt lines held by peripherals (level sources 0-5) */
    uint32_t irq_level;

    /* the analog blocks behind the ROM's rom_i2c_readReg/writeReg (BBPLL,
       PHY): [block][register], plain storage */
    uint8_t  analog[256][256];

    /* FRC1: 23-bit down counter */
    uint32_t t1_load, t1_ctrl;
    uint64_t t1_start;          /* tick the counter was (re)loaded */
    bool     t1_int;            /* interrupt status */
    uint64_t t1_fire;           /* next underflow, 0 none */
    /* FRC2: 32-bit up counter with an alarm */
    uint32_t t2_ctrl, t2_alarm, t2_base;
    uint64_t t2_start;
    bool     t2_int;
    uint64_t t2_fire;

    /* GPIO */
    uint32_t gpio_out, gpio_en, gpio_status, gpio_pin[16];
    uint32_t sigma_delta;
    uint32_t rtc_gpio_out, rtc_gpio_en;

    /* HSPI: busy until this tick */
    uint64_t spi1_busy_until;
    bool     spi1_pending;      /* started, not yet captured (see spi1_flush) */
    bool     spi1_done_due;     /* captured, finishes at spi1_busy_until */
    uint64_t spi1_cmd_tick;
    uint32_t spi_bits;          /* bits collected towards the next panel byte */
    int      spi_nbits;

    /* the board */
    I2cBus   i2c;
    OneWire  ow;
    uint8_t  buttons;           /* held, bit set = pressed: MCP23017 A0..A7 */
    St7735   lcd;
    uint32_t lcd_frames;        /* frame-sized writes, for the fps counter */

    /* WS2812 */
    bool     led_level;
    uint64_t led_rise;
    uint32_t led_bits;
    int      led_n;
    uint64_t led_last_edge;
    uint32_t led_rgb;           /* 0xRRGGBB as last latched */

    /* speaker: GPIO0 level changes, for audio.c */
    float    spk_level;
    SpkEvent spk_log[ESP_SPK_LOG];
    uint32_t spk_head, spk_tail;
};

extern bool esp_trace_io;
extern bool esp_lcd_log;      /* every panel command (and its parameters) to stderr */
extern bool esp_i2c_log;      /* every I2C byte to stderr */
extern uint32_t esp_watch;

/* esp8266.c */
Esp     *esp_create(void);
void     esp_destroy(Esp *s);
void     esp_reset(Esp *s, bool power_on);
uint32_t esp_read(Esp *s, uint32_t addr, int size);
void     esp_write(Esp *s, uint32_t addr, uint32_t v, int size);
void     esp_run(Esp *s, uint64_t ticks);
void     esp_set_cpu_hz(Esp *s, uint32_t hz);
void     esp_map_window(Esp *s);
void     esp_invalidate_iram(Esp *s, uint32_t off, int len);
void     esp_flash_changed(Esp *s, uint32_t off, uint32_t len);
XtInsn  *esp_decode_page(Esp *s, uint32_t pc);
void     esp_console_byte(Esp *s, uint8_t b);
void     esp_watch_hit(Esp *s, uint32_t a, uint32_t v, int size);
static inline uint64_t esp_now(const Esp *s) { return s->core.cycles; }

/* xt_cpu.c */
void     xt_reset(XtCore *c);
void     xt_run(Esp *s, XtCore *c, uint64_t until);
uint32_t xt_ccount(const XtCore *c);
void     xt_set_tick_mult(XtCore *c, uint32_t mult);
void     xt_irq_changed(Esp *s);
void     xt_trace_dump(void);
extern bool xt_trace;

/* periph.c */
void     periph_reset(Esp *s, bool power_on);
uint32_t periph_read(Esp *s, uint32_t addr);
void     periph_write(Esp *s, uint32_t addr, uint32_t v);
uint64_t periph_next_event(Esp *s);
void     periph_events(Esp *s);
void     gpio_update(Esp *s);
void     uart_irq(Esp *s);
/* bytes typed into UART0, as from a serial terminal */
void     esp_uart_input(Esp *s, const uint8_t *data, size_t n);

/* i2c.c */
void     i2c_reset(I2cBus *b, bool power_on);
void     i2c_lines(Esp *s, bool sda, bool scl);
bool     i2c_sda(const I2cBus *b);
uint8_t  mcp_port_b(const I2cBus *b);
float    backlight_level(const I2cBus *b);

/* onewire.c */
void     ow_reset(OneWire *w);
void     ow_line(Esp *s, bool esp_low);          /* the ESP pulls GPIO2 low or lets it go */
bool     ow_low(const Esp *s);                   /* the sensor pulls the line low now */

/* a piece of flash content: the SDK's default system parameters (sdk_params.c) */
typedef struct { uint32_t addr; uint16_t len; const uint8_t *data; } EspFlashBlock;
extern const EspFlashBlock esp_sdk_params[];
extern const size_t esp_sdk_params_count;

/* loader.c */
bool     esp_load_rom(Esp *s, const char *path, char *err, size_t errlen);
bool     esp_load_program(Esp *s, const char *path, char *err, size_t errlen);
bool     esp_load_program_mem(Esp *s, const uint8_t *data, size_t n, char *err, size_t errlen);
/* restores a save (the 4 MB flash a program left) past the program image */
bool     esp_apply_save(Esp *s, const uint8_t *data, size_t n);

/* ------------------------------------------------------------------------ */
/* Fast memory access                                                        */
/* ------------------------------------------------------------------------ */

#ifdef ESP_RWATCH
/* debugging builds (-DESP_RWATCH): loads in [esp_rwatch, esp_rwatch + esp_rwatch_len) are reported */
extern uint32_t esp_rwatch, esp_rwatch_len;
void esp_rwatch_hit(Esp *s, uint32_t a, int size);
#endif

static inline uint32_t mem_read(Esp *s, uint32_t a, int size)
{
#ifdef ESP_RWATCH
    if (a - esp_rwatch < esp_rwatch_len) esp_rwatch_hit(s, a, size);
#endif
    const uint8_t *p = s->rpage[a >> 16];
    if (p && ((a & 0xffff) + (uint32_t)size <= 0x10000)) {
        p += a & 0xffff;
        if (size == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
        if (size == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
        return *p;
    }
    return esp_read(s, a, size);
}

static inline void mem_write(Esp *s, uint32_t a, uint32_t v, int size)
{
    if ((uint32_t)(esp_watch - a) < (uint32_t)size) esp_watch_hit(s, a, v, size);
    uint8_t *p = s->wpage[a >> 16];
    if (p && ((a & 0xffff) + (uint32_t)size <= 0x10000)) {
        p += a & 0xffff;
        if (size == 4) memcpy(p, &v, 4);
        else if (size == 2) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }
        else *p = (uint8_t)v;
        if (s->wkind[a >> 16]) {
            const uint32_t off = a & 0xffff;
            if (s->iram_code[off >> 6] | s->iram_code[(off + (uint32_t)size - 1) >> 6])
                esp_invalidate_iram(s, off, size);
        }
        return;
    }
    esp_write(s, a, v, size);
}

#endif
