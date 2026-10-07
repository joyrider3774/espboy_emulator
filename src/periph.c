/*
 * The ESP8266's peripherals, register by register as the ROM, the NONOS SDK
 * and the Arduino core use them (esp8266_peri.h, eagle_soc.h and the ROM's
 * own code are the references):
 *
 *   0x3FF00000  DPORT: NMI source (bits 4:0 = 15: FRC1 is the NMI), edge
 *               interrupt enables, cache control (0x0C), CPU2X (0x14), eFuse
 *   0x3FF20C00  the WDEV counter, microseconds (system_get_time, micros())
 *   0x60000000  UART0, console. 0x60000F00 UART1, dropped
 *   0x60000100  SPI1 (HSPI): the display
 *   0x60000200  SPI0: the flash chip (ROM SPIRead/SPIWrite/SPIEraseSector)
 *   0x60000300  GPIO, sigma-delta, RTC calibration
 *   0x60000600  FRC1 (timer1: tone(), analogWrite, Servo through the core's
 *               waveform NMI) and FRC2 (the SDK's timers)
 *   0x60000700  RTC: reset cause, the slow counter, GPIO16 (the display's D/C)
 *   0x60000D00  the analog I2C master (rom_i2c_writeReg): plain storage
 *
 * Nothing is ticked: a timer's counter is worked out from the time it was
 * loaded, and its next interrupt is an event (periph_next_event) the run
 * loop stops at. Registers nothing models keep what was written.
 *
 * WiFi is not emulated: its registers (0x3FF20000, 0x60009000-) are plain
 * storage, which is enough for the SDK's start-up as long as WiFi stays off.
 */
#include <stdio.h>
#include <string.h>
#include "esp8266.h"

#define IO(off)  (*(uint32_t *)(s->io + (off)))

/* eFuse words of the attached ESPboy (MAC 08:f9:e0:79:e5:60, read with esptool) */
static const uint32_t efuse[4] = { 0x60660000u, 0x020079e5u, 0x2e00b000u, 0x0008f9e0u };

/* JEDEC ID of its flash chip (manufacturer 0x5E, device 0x4016: 4 MB) */
#define FLASH_JEDEC 0x0016405eu

bool esp_lcd_log = false;

static inline uint64_t now(const Esp *s) { return s->core.cycles; }

/* a peripheral changed when something happens next: the slice ends */
static inline void reschedule(Esp *s) { s->core.stop = true; }

static void set_level_irq(Esp *s, int line, bool on)
{
    const uint32_t bit = 1u << line;
    const uint32_t old = s->irq_level;
    s->irq_level = on ? old | bit : old & ~bit;
    if (s->irq_level != old) xt_irq_changed(s);
}

static void edge_irq(Esp *s, int line)
{
    s->core.interrupt |= 1u << line;
    xt_irq_changed(s);
}

/* ------------------------------------------------------------------------ */
/* Timers                                                                    */
/* ------------------------------------------------------------------------ */

/* ticks one count lasts at the divider in CTRL bits 3:2 (APB 80 MHz / 1, 16, 256) */
static uint32_t timer_tpc(uint32_t ctrl)
{
    static const uint32_t div[4] = { 1, 16, 256, 256 };
    return 2u * div[(ctrl >> 2) & 3];
}

static uint32_t t1_count(const Esp *s)
{
    if (!(s->t1_ctrl & 0x80)) return s->t1_load;
    const uint64_t n = (now(s) - s->t1_start) / timer_tpc(s->t1_ctrl);
    if (s->t1_ctrl & 0x40) {            /* auto reload: load .. 1, load .. */
        if (!s->t1_load) return 0;
        return s->t1_load - (uint32_t)(n % s->t1_load);
    }
    return (uint32_t)(s->t1_load - n) & 0x7fffffu;
}

static void t1_schedule(Esp *s)
{
    s->t1_fire = (s->t1_ctrl & 0x80) ? s->t1_start + (uint64_t)(s->t1_load ? s->t1_load : 0x800000u) * timer_tpc(s->t1_ctrl) : 0;
    reschedule(s);
}

static void t1_fire(Esp *s)
{
    s->t1_int = true;
    /* NmiTimSetFunc selects FRC1 as the NMI with DPORT bits 4:0 = 15 */
    if ((s->dport[0] & 0x1f) == 15) edge_irq(s, INUM_NMI);
    else if (s->dport[1] & 2) edge_irq(s, INUM_FRC1);
    if (s->t1_ctrl & 0x40) {
        s->t1_start = s->t1_fire;
        t1_schedule(s);
    } else {
        /* counts on from 0x7FFFFF down */
        s->t1_start = s->t1_fire;
        s->t1_load = 0x7fffffu;
        s->t1_fire = s->t1_start + (uint64_t)0x800000u * timer_tpc(s->t1_ctrl);
    }
}

static uint32_t t2_count(const Esp *s)
{
    if (!(s->t2_ctrl & 0x80)) return s->t2_base;
    return s->t2_base + (uint32_t)((now(s) - s->t2_start) / timer_tpc(s->t2_ctrl));
}

static void t2_schedule(Esp *s)
{
    if (!(s->t2_ctrl & 0x80)) { s->t2_fire = 0; return; }
    const uint32_t cnt = t2_count(s);
    const uint32_t tpc = timer_tpc(s->t2_ctrl);
    /* the counts already gone into the current one */
    const uint64_t frac = (now(s) - s->t2_start) % tpc;
    uint32_t delta = s->t2_alarm - cnt;
    if (!delta) delta = 0xffffffffu;    /* equality has just passed: next time round */
    s->t2_fire = now(s) - frac + (uint64_t)delta * tpc;
    reschedule(s);
}

static void t2_fire(Esp *s)
{
    s->t2_int = true;
    /* the SDK's timer interrupt (ets_timer_handler_isr); INTENABLE gates it */
    edge_irq(s, INUM_FRC2);
    s->t2_fire = s->t2_fire + (uint64_t)0x100000000ull * timer_tpc(s->t2_ctrl);
}

/* ------------------------------------------------------------------------ */
/* GPIO                                                                      */
/* ------------------------------------------------------------------------ */

static inline bool drives_low(const Esp *s, int pin)
{
    return (s->gpio_en >> pin & 1) && !(s->gpio_out >> pin & 1);
}

/* The level each pin is at: driven, or what the board does to it */
static uint32_t gpio_in(const Esp *s)
{
    uint32_t in = 0;
    for (int p = 0; p < 16; p++) {
        bool level;
        if (p == 4) level = s->i2c.sda;
        else if (p == 5) level = s->i2c.scl;
        else if (p == 2 && !(s->gpio_en >> 2 & 1)) level = !ow_low(s);    /* pulled up; the DS18B20 */
        else if (s->gpio_en >> p & 1) level = s->gpio_out >> p & 1;
        else level = p != 15;       /* pulled up on the D1 mini, GPIO15 pulled down */
        in |= (uint32_t)level << p;
    }
    /* bits 31:16: the boot strapping latched at reset. Bits 18:16 GPIO15=0,
       GPIO0=1, GPIO2=1: boot from flash; the ROM prints them and bits 31:29
       as "boot mode:(3,6)", what the ESPboy shows */
    return in | (uint32_t)0xC003u << 16;
}

static void speaker_set(Esp *s, float level)
{
    if (level == s->spk_level) return;
    s->spk_level = level;
    const uint32_t next = (s->spk_head + 1) % ESP_SPK_LOG;
    if (next == s->spk_tail) return;    /* the front end is not taking sound: drop */
    s->spk_log[s->spk_head].tick = now(s);
    s->spk_log[s->spk_head].level = level;
    s->spk_head = next;
}

/* WS2812: a high pulse longer than 0.6 us is a 1; 24 bits are G R B, a low
   time over 50 us latches */
static void led_edge(Esp *s, bool level)
{
    const uint64_t t = now(s);
    if (t - s->led_last_edge > 50u * 160u) s->led_n = 0, s->led_bits = 0;
    s->led_last_edge = t;
    if (level) { s->led_rise = t; return; }
    const uint64_t high = t - s->led_rise;
    s->led_bits = s->led_bits << 1 | (high > 96u);   /* 0.6 us at 160 MHz */
    if (++s->led_n == 24) {
        const uint32_t g = s->led_bits >> 16 & 0xff, r = s->led_bits >> 8 & 0xff, b = s->led_bits & 0xff;
        s->led_rgb = r << 16 | g << 8 | b;
        s->led_n = 0;
        s->led_bits = 0;
    }
}

/* Something changed what the pins drive: the I2C lines, the LED data line
   and the speaker follow */
void gpio_update(Esp *s)
{
    /* SDA and SCL are open drain with pull-ups: low when the ESP or a slave pulls */
    const bool msda = !drives_low(s, 4);
    const bool scl = !drives_low(s, 5);
    if (msda != s->i2c.msda || scl != s->i2c.scl) {
        s->i2c.msda = msda;
        i2c_lines(s, msda, scl);
    }

    const bool led = (s->gpio_en >> 2 & 1) ? (s->gpio_out >> 2 & 1) : true;
    if (led != s->led_level) { s->led_level = led; led_edge(s, led); }
    ow_line(s, drives_low(s, 2));

    /* GPIO0: the speaker. Its pin may take the sigma-delta modulator's output
       (GPIO_PIN0 bit 0), which averages to target/256 */
    float level = 0;
    if (s->gpio_en & 1) {
        if ((s->gpio_pin[0] & 1) && (s->sigma_delta & 0x10000))
            level = (float)(s->sigma_delta & 0xff) / 256.0f;
        else
            level = (float)(s->gpio_out & 1);
    }
    speaker_set(s, level);
}

/* ------------------------------------------------------------------------ */
/* SPI1 (HSPI): the display                                                  */
/* ------------------------------------------------------------------------ */

/* the panel's chip select is MCP23017 B0, low = selected */
static bool lcd_selected(const Esp *s)
{
    return !(mcp_port_b(&s->i2c) & 1);
}

static void lcd_bits(Esp *s, uint32_t value, int n)
{
    if (!lcd_selected(s)) { s->spi_nbits = 0; return; }
    /* D/C is GPIO16, in the RTC block: high = data */
    const bool dc = (s->rtc_gpio_en & 1) ? (s->rtc_gpio_out & 1) : true;
    for (int i = n - 1; i >= 0; i--) {
        s->spi_bits = s->spi_bits << 1 | (value >> i & 1);
        if (++s->spi_nbits == 8) {
            if (esp_lcd_log && (!dc || !s->lcd.writing))
                fprintf(stderr, "[lcd %.6f] %s %02x\n", (double)now(s) / ESP_TICK_HZ, dc ? "  data" : "cmd", s->spi_bits & 0xff);
            st7735_byte(&s->lcd, dc, (uint8_t)s->spi_bits);
            s->spi_nbits = 0;
        }
    }
}

static void spi1_start(Esp *s, uint64_t at)
{
    const uint32_t user = IO(0x11C), u1 = IO(0x120), u2 = IO(0x124), ctrl = IO(0x108), clk = IO(0x118);
    uint32_t bits = 0;
    if (user & (1u << 31)) {            /* command phase */
        const int n = (int)((u2 >> 28) & 15) + 1;
        uint32_t v = u2 & 0xffff;
        if (n > 8) v = ((v & 0xff) << 8) | (v >> 8);     /* sent low byte first */
        lcd_bits(s, v, n);
        bits += (uint32_t)n;
    }
    if (user & (1u << 30)) {            /* address phase, from the top of SPI_ADDR */
        const int n = (int)((u1 >> 26) & 63) + 1;
        lcd_bits(s, IO(0x104) >> (32 - n), n);
        bits += (uint32_t)n;
    }
    if (user & (1u << 29)) bits += (u1 & 0xff) + 1;     /* dummy */
    uint32_t mosi = 0, miso = 0;
    if (user & (1u << 27)) {            /* MOSI: from W0 (W8 with USR_MOSI_HIGHPART) */
        mosi = ((u1 >> 17) & 0x1ff) + 1;
        const uint32_t base = (user & (1u << 25)) ? 0x160 : 0x140;
        const bool big = (user & (1u << 11)) != 0, lsb = (ctrl & (1u << 26)) != 0;
        for (uint32_t k = 0; k * 8 < mosi && k < 64; k++) {
            const uint32_t w = IO(base + (k / 4) * 4);
            uint32_t b = (w >> ((big ? 3 - (k & 3) : (k & 3)) * 8)) & 0xff;
            if (lsb) {
                uint32_t r = 0;
                for (int i = 0; i < 8; i++) r |= ((b >> i) & 1) << (7 - i);
                b = r;
            }
            const int n = mosi - k * 8 >= 8 ? 8 : (int)(mosi - k * 8);
            lcd_bits(s, b >> (8 - n), n);
        }
    }
    if (user & (1u << 28)) {            /* MISO: into W0 (W8 with USR_MISO_HIGHPART) */
        miso = ((u1 >> 8) & 0x1ff) + 1;
        const uint32_t base = (user & (1u << 24)) ? 0x160 : 0x140;
        for (uint32_t k = 0; k * 32 < miso && k < 16; k++) IO(base + k * 4) = 0;
        /* MISO (GPIO12) is not wired to the panel; in 3-wire mode (SIO) the
           ESP reads the panel's SDA on MOSI. Bytes fill each word from its low
           byte, MSB first (a partial last byte keeps its bits low) */
        if ((user & (1u << 16)) && lcd_selected(s)) {
            const bool big = (user & (1u << 10)) != 0, lsb = (ctrl & (1u << 25)) != 0;
            for (uint32_t k = 0; k * 8 < miso && k < 64; k++) {
                const int n = miso - k * 8 >= 8 ? 8 : (int)(miso - k * 8);
                uint32_t b = 0;
                for (int i = 0; i < n; i++) b = b << 1 | (uint32_t)st7735_read_bit(&s->lcd);
                if (lsb) {
                    uint32_t r = 0;
                    for (int i = 0; i < n; i++) r |= ((b >> i) & 1) << (n - 1 - i);
                    b = r;
                }
                IO(base + (k / 4) * 4) |= b << ((big ? 3 - (k & 3) : (k & 3)) * 8);
            }
        }
    }
    bits += (user & 1) ? (mosi > miso ? mosi : miso) : mosi + miso;
    const uint32_t tpb = (clk & (1u << 31)) ? 2u
                       : 2u * (((clk >> 18) & 0x1fff) + 1) * (((clk >> 12) & 0x3f) + 1);
    s->spi1_busy_until = at + (uint64_t)bits * tpb + 8;
    s->spi1_done_due = true;
    reschedule(s);
}

/* SPI_SLAVE (+0x30) bit 4 TRANS_DONE, bit 9 its interrupt enable: the HSPI
   interrupt (line 2, DPORT 0x3FF00020 bit 7) */
static bool spi1_irq(const Esp *s)
{
    return (IO(0x130) & 0x210) == 0x210;
}

static void spi1_done(Esp *s)
{
    s->spi1_done_due = false;
    IO(0x100) &= ~(1u << 18);
    IO(0x130) |= 1u << 4;
    set_level_irq(s, INUM_SPI, spi1_irq(s));
}

/* A transfer started by SPI_CMD takes its lengths and data a few cycles
   later: LovyanGFX (writeDataRepeat) starts one and only then writes
   SPI_USER1 with its length, and that length is what goes out. So the
   transfer is captured at the next access to a peripheral, except a write
   of SPI_USER/USER1 straight after the start */
#define SPI1_LATCH_TICKS 32

static void spi1_flush(Esp *s)
{
    if (!s->spi1_pending) return;
    s->spi1_pending = false;
    spi1_start(s, s->spi1_cmd_tick);
}

/* ------------------------------------------------------------------------ */
/* SPI0: the flash chip                                                      */
/* ------------------------------------------------------------------------ */

static void flash_program(Esp *s, uint32_t addr, const uint8_t *data, uint32_t n)
{
    /* NOR: bits only go from 1 to 0, and a page program wraps in its 256 bytes */
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t a = (addr & ~0xffu) | ((addr + i) & 0xffu);
        if (a < ESP_FLASH_SIZE) s->flash[a] &= data[i];
    }
    esp_flash_changed(s, addr & ~0xffu, 256);
}

static void flash_erase(Esp *s, uint32_t addr, uint32_t size)
{
    addr &= ~(size - 1);
    if (addr >= ESP_FLASH_SIZE) return;
    if (addr + size > ESP_FLASH_SIZE) size = ESP_FLASH_SIZE - addr;
    memset(s->flash + addr, 0xff, size);
    esp_flash_changed(s, addr, size);
}

static void w_from_flash(Esp *s, uint32_t base, uint32_t addr, uint32_t n)
{
    for (uint32_t i = 0; i < n && i < 64; i++)
        s->io[base + i] = (addr + i) < ESP_FLASH_SIZE ? s->flash[(addr + i)] : 0xff;
}

/* a command of the flash chip, with its address and data (SPI0 user commands) */
static void flash_command(Esp *s, uint8_t cmd, uint32_t addr, uint32_t mosi_bytes, uint32_t miso_bytes)
{
    switch (cmd) {
    case 0x03: case 0x0B: case 0x3B: case 0xBB: case 0x6B: case 0xEB:
        w_from_flash(s, 0x240, addr, miso_bytes);
        break;
    case 0x9F: IO(0x240) = FLASH_JEDEC; break;
    case 0x05: IO(0x240) = s->flash_status; break;
    case 0x35: case 0x15: IO(0x240) = 0; break;
    case 0x06: s->flash_status |= 2; break;
    case 0x04: s->flash_status &= (uint8_t)~2; break;
    case 0x01: s->flash_status &= (uint8_t)~2; break;
    case 0x02: flash_program(s, addr, s->io + 0x240, mosi_bytes > 64 ? 64 : mosi_bytes); s->flash_status &= (uint8_t)~2; break;
    case 0x20: flash_erase(s, addr, 0x1000); s->flash_status &= (uint8_t)~2; break;
    case 0x52: flash_erase(s, addr, 0x8000); s->flash_status &= (uint8_t)~2; break;
    case 0xD8: flash_erase(s, addr, 0x10000); s->flash_status &= (uint8_t)~2; break;
    case 0x60: case 0xC7: flash_erase(s, 0, ESP_FLASH_SIZE); s->flash_status &= (uint8_t)~2; break;
    case 0x4B: IO(0x240) = 0x79e5f908u; IO(0x244) = 0x60e00000u; break;     /* unique ID */
    case 0x5A: memset(s->io + 0x240, 0xff, 64); break;                      /* no SFDP */
    default:
        if (esp_trace_io) fprintf(stderr, "[%08x] flash command %02x at %06x\n", s->core.pc, cmd, addr);
        break;
    }
}

static void spi0_cmd(Esp *s, uint32_t v)
{
    const uint32_t a = IO(0x204);
    const uint32_t addr = a & 0xffffff, len = a >> 24;
    if (v & (1u << 31)) w_from_flash(s, 0x240, addr, len ? len : 0);    /* READ */
    if (v & (1u << 30)) s->flash_status |= 2;                           /* WREN */
    if (v & (1u << 29)) s->flash_status &= (uint8_t)~2;                 /* WRDI */
    if (v & (1u << 28)) IO(0x240) = FLASH_JEDEC;                        /* RDID */
    if (v & (1u << 27)) IO(0x210) = (IO(0x210) & ~0xffu) | s->flash_status;   /* RDSR */
    if (v & (1u << 26)) { s->flash_status = (uint8_t)(IO(0x210) & 0xfc); }    /* WRSR */
    if (v & (1u << 25)) flash_command(s, 0x02, addr, len, 0);           /* PP */
    if (v & (1u << 24)) flash_command(s, 0x20, addr, 0, 0);             /* SE */
    if (v & (1u << 23)) flash_command(s, 0xD8, addr, 0, 0);             /* BE */
    if (v & (1u << 22)) flash_command(s, 0xC7, 0, 0, 0);                /* CE */
    if (v & (1u << 18)) {                                               /* USR */
        const uint32_t user = IO(0x21C), u1 = IO(0x220), u2 = IO(0x224);
        const uint8_t cmd = (user & (1u << 31)) ? (uint8_t)(u2 & 0xff) : 0;
        const uint32_t mosi = (user & (1u << 27)) ? ((((u1 >> 17) & 0x1ff) + 1) / 8) : 0;
        const uint32_t miso = (user & (1u << 28)) ? ((((u1 >> 8) & 0x1ff) + 1) / 8) : 0;
        uint32_t uaddr = 0;
        if (user & (1u << 30)) {
            const int n = (int)((u1 >> 26) & 63) + 1;
            uaddr = n >= 32 ? a : (n == 24 ? (a & 0xffffff) : a >> (32 - n));
        }
        flash_command(s, cmd, uaddr & 0xffffff, mosi, miso);
    }
    IO(0x200) = 0;      /* done at once */
}

/* ------------------------------------------------------------------------ */
/* Reset                                                                     */
/* ------------------------------------------------------------------------ */

void periph_reset(Esp *s, bool power_on)
{
    for (int i = 0; i < 4; i++) s->dport[0x50 / 4 + i] = efuse[i];
    s->t1_load = s->t1_ctrl = 0; s->t1_int = false; s->t1_fire = 0; s->t1_start = now(s);
    s->t2_ctrl = s->t2_alarm = s->t2_base = 0; s->t2_int = false; s->t2_fire = 0; s->t2_start = now(s);
    s->gpio_out = s->gpio_en = s->gpio_status = 0;
    memset(s->gpio_pin, 0, sizeof s->gpio_pin);
    s->sigma_delta = 0;
    s->rtc_gpio_out = s->rtc_gpio_en = 0;
    s->spi1_busy_until = 0;
    s->spi1_done_due = s->spi1_pending = false;
    s->spi_nbits = 0;
    s->flash_status = 0;
    s->led_level = true;
    ow_reset(&s->ow);
    s->uart_rx_head = s->uart_rx_tail = 0;
    if (power_on || !s->rng) s->rng = 0x9E3779B9u;
    memset(s->analog, 0, sizeof s->analog);
    /* UART0: 115200 at 26 MHz... the ROM sets its own divider; CONF0 8N1 */
    IO(0x014) = 0x2b6;
    IO(0x020) = 0x1c;
    /* RTC: the reset cause, 1 = power on, 2 = external, 4 = software */
    IO(0x714) = power_on ? 1 : (s->reset_request ? s->reset_request : 4);
    gpio_update(s);
}

/* ------------------------------------------------------------------------ */
/* Register access                                                           */
/* ------------------------------------------------------------------------ */

/* UART0's raw interrupt bits: TX FIFO empty (bit 1, always: output goes out
   at once), RX FIFO full (bit 0, at the CONF1 threshold) and RX timeout
   (bit 8, when enabled and anything waits: there is no line idle time here) */
static uint32_t uart0_raw(const Esp *s)
{
    uint32_t raw = IO(0x004) | 2u;
    const uint32_t n = (s->uart_rx_head - s->uart_rx_tail) & 255u, conf1 = IO(0x024);
    if (n && n >= (conf1 & 0x7f)) raw |= 1u;
    if (n && (conf1 & (1u << 31))) raw |= 1u << 8;
    return raw;
}

void uart_irq(Esp *s)
{
    /* the line is the OR of both UARTs */
    const uint32_t st0 = uart0_raw(s) & IO(0x00C), st1 = (IO(0xF04) | 2u) & IO(0xF0C);
    set_level_irq(s, INUM_UART, (st0 | st1) != 0);
}

uint32_t periph_read(Esp *s, uint32_t addr)
{
    spi1_flush(s);
    if ((addr >> 8) == 0x3FF000) {
        const uint32_t off = addr & 0xff;
        uint32_t v = s->dport[off / 4];
        switch (off) {
        case 0x0C:      /* cache: a flush (bit 0) is done at once (bit 1); SPI never busy (bit 9) */
            v = (v & ~0x202u) | ((v & 1) << 1);
            break;
        case 0x20: v = spi1_irq(s) ? 1u << 7 : 0; break;    /* SPI0 (4), SPI1 (7), I2S (9) interrupts */
        }
        return v;
    }
    if ((addr >> 16) == 0x3FF2) {
        const uint32_t off = addr & 0xffff;
        if (off == 0x0C00) return (uint32_t)(now(s) / (ESP_TICK_HZ / 1000000u));
        if (off == 0x0E44) {        /* the hardware random number generator (RANDOM_REG32, os_random) */
            uint32_t x = s->rng;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            s->rng = x;
            return x;
        }
        return *(uint32_t *)(s->wifi + off);
    }

    const uint32_t off = addr & 0xffff;
    /* the RTC block is in its own slow clock domain: a read takes about 63
       cycles at 160 MHz (measured), 47 ticks more than other peripherals */
    if ((off & 0xff00) == 0x700) s->core.cycles += 47;
    switch (off) {
    /* UART0 / UART1 */
    case 0x000:                 /* RX FIFO */
        if (s->uart_rx_head != s->uart_rx_tail) {
            const uint8_t b = s->uart_rx[s->uart_rx_tail++ & 255u];
            uart_irq(s);
            return b;
        }
        return 0;
    case 0xF00: return 0;
    case 0x004: return uart0_raw(s);
    case 0x008: return uart0_raw(s) & IO(0x00C);
    case 0xF04: return IO(off) | 2u;                       /* INT_RAW: TX FIFO empty */
    case 0xF08: return (IO(0xF04) | 2u) & IO(0xF0C);
    case 0x01C: return (s->uart_rx_head - s->uart_rx_tail) & 255u;   /* STATUS: RX count, TX FIFO empty */
    case 0xF1C: return 0;
    /* SPI1 */
    case 0x100:
        if (s->spi1_done_due && now(s) >= s->spi1_busy_until) spi1_done(s);
        return IO(0x100);
    /* GPIO */
    case 0x300: return s->gpio_out;
    case 0x30C: return s->gpio_en;
    case 0x318: return gpio_in(s);
    case 0x31C: return s->gpio_status;
    case 0x368: return s->sigma_delta;
    case 0x370:     /* RTC calibration result: ready, xtal cycles over the slow clock periods asked for */
        return (1u << 31) | (((IO(0x36C) & 0x3ff) * (26000000u / 150000u)) & 0xfffff);
    /* timers */
    case 0x600: return s->t1_load;
    case 0x604: return t1_count(s);
    case 0x608: return s->t1_ctrl | (s->t1_int ? 0x100u : 0);
    case 0x620: return IO(0x620);
    case 0x624: return t2_count(s);
    case 0x628: return s->t2_ctrl | (s->t2_int ? 0x100u : 0);
    case 0x630: return s->t2_alarm;
    /* RTC */
    case 0x71C: return (uint32_t)(now(s) / (ESP_TICK_HZ / 150000u));    /* the ~150 kHz slow clock */
    case 0x728: return IO(0x728) | 1;   /* wake-up status (pm_wait4wakeup): the chip never sleeps here */
    /* PHY calibration waits on these (WiFi is not emulated; the SDK starts the PHY anyway) */
    case 0x57C: return IO(off) | (1u << 31);    /* rom_iq_est_enable: estimate done */
    case 0x9B60: return IO(off) & ~2u;          /* a PHY measurement started with bit 1: finished */
    case 0x768: return s->rtc_gpio_out;
    case 0x774: return s->rtc_gpio_en;
    case 0x78C: return (s->rtc_gpio_en & 1) ? (s->rtc_gpio_out & 1) : 1;
    /* the analog I2C master: never busy (bit 25) */
    case 0xD00: case 0xD04: case 0xD08: case 0xD0C: return IO(off) & ~(1u << 25);
    }
    if (off >= 0x328 && off < 0x368) return s->gpio_pin[(off - 0x328) / 4];
    if (off < 0x2000) return IO(off);
    if (esp_trace_io) fprintf(stderr, "[%08x] read %08x\n", s->core.pc, addr);
    return IO(off);
}

void periph_write(Esp *s, uint32_t addr, uint32_t v)
{
    if (s->spi1_pending && !((addr == 0x60000120u || addr == 0x6000011Cu) &&
                             now(s) - s->spi1_cmd_tick <= SPI1_LATCH_TICKS))
        spi1_flush(s);
    if ((addr >> 8) == 0x3FF000) {
        const uint32_t off = addr & 0xff;
        const uint32_t old = s->dport[off / 4];
        switch (off) {
        case 0x50: case 0x54: case 0x58: case 0x5C: return;     /* eFuse: read only */
        }
        s->dport[off / 4] = v;
        if (off == 0x0C && ((old ^ v) & 0x03070000u)) esp_map_window(s);
        if (off == 0x14) esp_set_cpu_hz(s, (v & 1) ? 160000000u : 80000000u);
        if (off == 0x00 || off == 0x04) reschedule(s);
        return;
    }
    if ((addr >> 16) == 0x3FF2) {
        *(uint32_t *)(s->wifi + (addr & 0xffff)) = v;
        return;
    }

    const uint32_t off = addr & 0xffff;
    switch (off) {
    /* UART0: the console. UART1 (TX on GPIO2, the LED) is dropped */
    case 0x000: esp_console_byte(s, (uint8_t)v); return;
    case 0xF00: return;
    case 0x00C: case 0xF0C: IO(off) = v; uart_irq(s); return;
    case 0x010: case 0xF10: IO(off - 0xC) &= ~v; uart_irq(s); return;
    /* SPI1 */
    case 0x100:
        IO(0x100) = v;
        if (v & (1u << 18)) {
            s->spi1_pending = true;
            s->spi1_cmd_tick = now(s);
            s->spi1_busy_until = UINT64_MAX;
        }
        return;
    case 0x130:     /* SPI_SLAVE: TRANS_DONE (bit 4) is cleared by writing 0, bit 9 enables its interrupt */
        IO(0x130) = v;
        set_level_irq(s, INUM_SPI, spi1_irq(s));
        return;
    /* SPI0 */
    case 0x200: spi0_cmd(s, v); return;
    /* GPIO */
    case 0x300: s->gpio_out = v & 0xffff; gpio_update(s); return;
    case 0x304: s->gpio_out |= v & 0xffff; gpio_update(s); return;
    case 0x308: s->gpio_out &= ~v; gpio_update(s); return;
    case 0x30C: s->gpio_en = v & 0xffff; gpio_update(s); return;
    case 0x310: s->gpio_en |= v & 0xffff; gpio_update(s); return;
    case 0x314: s->gpio_en &= ~v; gpio_update(s); return;
    case 0x31C: s->gpio_status = v & 0xffff; set_level_irq(s, INUM_GPIO, s->gpio_status != 0); return;
    case 0x320: s->gpio_status |= v & 0xffff; set_level_irq(s, INUM_GPIO, s->gpio_status != 0); return;
    case 0x324: s->gpio_status &= ~v; set_level_irq(s, INUM_GPIO, s->gpio_status != 0); return;
    case 0x368: s->sigma_delta = v; gpio_update(s); return;
    /* FRC1 */
    case 0x600:
        s->t1_load = v & 0x7fffff;
        s->t1_start = now(s);
        t1_schedule(s);
        return;
    case 0x608: {
        const bool was = s->t1_ctrl & 0x80;
        s->t1_ctrl = v & 0xcf;
        if (!was && (v & 0x80)) s->t1_start = now(s);
        t1_schedule(s);
        return;
    }
    case 0x60C: s->t1_int = false; return;
    /* FRC2 */
    case 0x620:
        IO(0x620) = v;
        s->t2_base = v;
        s->t2_start = now(s);
        t2_schedule(s);
        return;
    case 0x628: {
        const uint32_t cnt = t2_count(s);
        s->t2_base = cnt;
        s->t2_start = now(s);
        s->t2_ctrl = v & 0xcf;
        t2_schedule(s);
        return;
    }
    case 0x62C: s->t2_int = false; return;
    case 0x630: s->t2_alarm = v; t2_schedule(s); return;
    /* RTC */
    case 0x700:
        IO(off) = v;
        if (v & (1u << 31)) s->reset_request = 4;   /* software reset */
        return;
    case 0x768: s->rtc_gpio_out = v; return;
    case 0x774: s->rtc_gpio_en = v; return;
    /* the analog I2C master: a write (bit 24) stores, a read gets the register in bits 23:16 */
    case 0xD00: case 0xD04: case 0xD08: case 0xD0C: {
        const uint8_t block = (uint8_t)v, reg = (uint8_t)(v >> 8);
        if (v & (1u << 24)) s->analog[block][reg] = (uint8_t)(v >> 16);
        uint8_t got = s->analog[block][reg];
        /* status bits the SDK's PHY start-up waits for: block 0x62 register 7
           bit 7 is the RF PLL calibration finishing (libphy wait_rfpll_cal_end,
           "pll_cal exceeds 2ms" when it never does) */
        if (block == 0x62 && reg == 7) got |= 0x80;
        IO(off) = (v & ~0x02ff0000u) | (uint32_t)got << 16;
        return;
    }
    }
    if (off >= 0x328 && off < 0x368) { s->gpio_pin[(off - 0x328) / 4] = v; gpio_update(s); return; }
    if (off >= 0x2000 && esp_trace_io) fprintf(stderr, "[%08x] write %08x to %08x\n", s->core.pc, v, addr);
    IO(off) = v;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

uint64_t periph_next_event(Esp *s)
{
    uint64_t next = UINT64_MAX;
    if (s->t1_fire && s->t1_fire < next) next = s->t1_fire;
    if (s->t2_fire && s->t2_fire < next) next = s->t2_fire;
    if (s->spi1_done_due && s->spi1_busy_until < next) next = s->spi1_busy_until;
    return next;
}

void periph_events(Esp *s)
{
    spi1_flush(s);
    const uint64_t t = now(s);
    while (s->t1_fire && s->t1_fire <= t) t1_fire(s);
    while (s->t2_fire && s->t2_fire <= t) t2_fire(s);
    if (s->spi1_done_due && s->spi1_busy_until <= t) spi1_done(s);
}

void esp_uart_input(Esp *s, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((uint8_t)(s->uart_rx_head + 1) == s->uart_rx_tail) break;     /* FIFO full: dropped */
        s->uart_rx[s->uart_rx_head++] = data[i];
    }
    uart_irq(s);
}
