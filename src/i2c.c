/*
 * The ESPboy's I2C bus. The ESP8266 has no I2C controller: the Arduino core
 * (and LovyanGFX) bit-bang it on GPIO4 (SDA) and GPIO5 (SCL), open drain,
 * so the bus is decoded here from the line levels, edge by edge, as the
 * chips on it see it:
 *
 *   0x20  MCP23017 16-bit I/O expander (Microchip DS20001952). Port A: the
 *         eight buttons, to ground when pressed (the program enables the
 *         pull-ups). Port B: B0 the display's chip select, B1 the LED lock.
 *   0x60  MCP4725 12-bit DAC (DS22039): the backlight.
 *
 * START is SDA falling while SCL is high, STOP SDA rising while SCL is high.
 * Data bits are sampled on SCL rising; a slave changes SDA only while SCL is
 * low (on SCL falling), for its ACKs and for the bytes it sends.
 */
#include <stdio.h>
#include <string.h>
#include "esp8266.h"

enum { ST_IDLE, ST_ADDR, ST_WRITE, ST_ACK_OUT, ST_READ, ST_READ_ACK, ST_IGNORE };
enum { DEV_NONE = -1, DEV_MCP = 0, DEV_DAC = 1 };

/* MCP23017 registers (IOCON.BANK = 0) */
enum {
    IODIRA = 0x00, IODIRB = 0x01, IPOLA = 0x02, IPOLB = 0x03, IOCON = 0x0A, GPPUA = 0x0C, GPPUB = 0x0D,
    INTFA = 0x0E, INTCAPA = 0x10, GPIOA = 0x12, GPIOB = 0x13, OLATA = 0x14, OLATB = 0x15,
};

void i2c_reset(I2cBus *b, bool power_on)
{
    const uint16_t eeprom = b->dac_eeprom;
    const uint8_t eeprom_pd = b->dac_eeprom_pd;
    memset(b, 0, sizeof(*b));
    b->sda = b->scl = b->msda = true;
    b->dev = DEV_NONE;
    /* power-on reset: IODIR all inputs, everything else 0 */
    b->mcp[IODIRA] = b->mcp[IODIRB] = 0xff;
    /* the DAC starts from what its EEPROM holds. The ESPboy library leaves
       4095 there (writeDAC(4095, true) at the end of begin()) */
    b->dac_eeprom = power_on ? 4095 : eeprom;
    b->dac_eeprom_pd = power_on ? 0 : eeprom_pd;
    b->dac = b->dac_eeprom;
    b->dac_pd = b->dac_eeprom_pd;
}

/* ------------------------------------------------------------------------ */
/* MCP23017                                                                  */
/* ------------------------------------------------------------------------ */

/* the level on each pin of a port: an output drives its latch, an input is
   what is outside (buttons on A) or its pull-up */
static uint8_t mcp_pins(const I2cBus *b, int port, uint8_t buttons)
{
    const uint8_t dir = b->mcp[IODIRA + port], olat = b->mcp[OLATA + port], pu = b->mcp[GPPUA + port];
    uint8_t outside = pu;                       /* nothing connected: the pull-up, or low */
    if (port == 0) outside &= (uint8_t)~buttons;  /* a pressed button pulls to ground */
    return (uint8_t)((olat & ~dir) | (outside & dir));
}

/* Port B's levels as the board sees them. A pin that is still an input
   floats; the display's chip select (B0) then reads as selected: programs
   that send the panel's init sequence before making B0 an output (some
   Gamebuino META ports) show a picture on the ESPboy */
uint8_t mcp_port_b(const I2cBus *b)
{
    const uint8_t dir = b->mcp[IODIRB];
    return (uint8_t)((b->mcp[OLATB] & ~dir) | (b->mcp[GPPUB] & dir));
}

static uint8_t mcp_read(I2cBus *b, uint8_t buttons)
{
    const uint8_t r = b->mcp_ptr;
    uint8_t v = r < 0x16 ? b->mcp[r] : 0;
    if (r == GPIOA || r == GPIOB) {
        const int port = r - GPIOA;
        v = mcp_pins(b, port, buttons) ^ (b->mcp[IPOLA + port] & b->mcp[IODIRA + port]);
    } else if (r == INTCAPA || r == INTCAPA + 1) {
        v = mcp_pins(b, r - INTCAPA, buttons);
    }
    /* sequential mode (IOCON.SEQOP = 0): the pointer moves on, wrapping */
    if (!(b->mcp[IOCON] & 0x20)) b->mcp_ptr = (uint8_t)((r + 1) % 0x16);
    return v;
}

static void mcp_write(I2cBus *b, uint8_t v)
{
    if (b->mcp_first) {
        b->mcp_ptr = v;
        b->mcp_first = false;
        return;
    }
    uint8_t r = b->mcp_ptr;
    if (r == GPIOA || r == GPIOB) r += 2;       /* writing GPIO writes OLAT */
    if (r == IOCON || r == IOCON + 1) {
        b->mcp[IOCON] = b->mcp[IOCON + 1] = v & 0xfe;
        if (v & 0x80) fprintf(stderr, "MCP23017: IOCON.BANK=1 is not modelled\n");
    } else if (r < 0x16 && r != INTFA && r != INTFA + 1 && r != INTCAPA && r != INTCAPA + 1) {
        b->mcp[r] = v;
    }
    if (!(b->mcp[IOCON] & 0x20)) b->mcp_ptr = (uint8_t)((b->mcp_ptr + 1) % 0x16);
}

/* ------------------------------------------------------------------------ */
/* MCP4725                                                                   */
/* ------------------------------------------------------------------------ */

static void dac_write(I2cBus *b, uint8_t v)
{
    if (b->dac_n < 3) b->dac_buf[b->dac_n] = v;
    b->dac_n++;
    const uint8_t c = b->dac_buf[0];
    if ((c >> 6) == 0) {
        /* fast mode: PD1 PD0 D11-D8, then D7-D0; pairs may repeat */
        if (b->dac_n == 2) {
            b->dac_pd = (c >> 4) & 3;
            b->dac = (uint16_t)((c & 15) << 8 | b->dac_buf[1]);
            b->dac_n = 0;
        }
    } else if (b->dac_n == 3) {
        /* C2 C1 C0 x x PD1 PD0 x, D11-D4, D3-D0 x x x x */
        const unsigned cmd = c >> 5;
        if (cmd == 2 || cmd == 3) {
            b->dac_pd = (c >> 1) & 3;
            b->dac = (uint16_t)(b->dac_buf[1] << 4 | b->dac_buf[2] >> 4);
            if (cmd == 3) { b->dac_eeprom = b->dac; b->dac_eeprom_pd = b->dac_pd; }
        }
        b->dac_n = 0;
    }
}

static uint8_t dac_read(I2cBus *b)
{
    /* status (RDY, POR, PD), DAC D11-D4, D3-D0, EEPROM PD + D11-D8, D7-D0 */
    const uint8_t bytes[5] = {
        (uint8_t)(0xc0 | b->dac_pd << 1), (uint8_t)(b->dac >> 4), (uint8_t)(b->dac << 4),
        (uint8_t)(b->dac_eeprom_pd << 5 | b->dac_eeprom >> 8), (uint8_t)b->dac_eeprom,
    };
    const int i = b->dac_rd < 5 ? b->dac_rd : 4;
    b->dac_rd++;
    return bytes[i];
}

/* The backlight, 0..1, from the DAC. The DAC drives the backlight LED's
   transistor: below about 250 nothing lights; LovyanGFX's full brightness
   is 1525 and TFT_eSPI programs use 4095. An estimate, to be compared with
   the real ESPboy */
float backlight_level(const I2cBus *b)
{
    if (b->dac_pd) return 0;
    float v = ((float)b->dac - 250.0f) / (1525.0f - 250.0f);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* ------------------------------------------------------------------------ */
/* The bus                                                                   */
/* ------------------------------------------------------------------------ */

bool esp_i2c_log = false;

static void start_byte_out(I2cBus *b, uint8_t buttons)
{
    if (esp_i2c_log && b->dev == DEV_MCP) fprintf(stderr, "[i2c] read reg %02x", b->mcp_ptr);
    b->byte = b->dev == DEV_MCP ? mcp_read(b, buttons) : dac_read(b);
    if (esp_i2c_log) fprintf(stderr, " -> %02x\n", b->byte);
    b->bit = 0;
    b->slave_low = !(b->byte & 0x80);
    b->state = ST_READ;
}

static void scl_falling(I2cBus *b, uint8_t buttons)
{
    switch (b->state) {
    case ST_ADDR:
        if (b->bit == 8) {
            const uint8_t a = b->byte >> 1;
            b->read = b->byte & 1;
            b->dev = a == 0x20 ? DEV_MCP : a == 0x60 ? DEV_DAC : DEV_NONE;
            if (b->dev == DEV_NONE) { b->state = ST_IGNORE; break; }
            if (b->dev == DEV_MCP) b->mcp_first = !b->read;
            if (b->dev == DEV_DAC) { if (b->read) b->dac_rd = 0; else b->dac_n = 0; }
            b->slave_low = true;        /* ACK */
            b->state = ST_ACK_OUT;
            b->transactions++;
        }
        break;
    case ST_WRITE:
        if (b->bit == 8) {
            if (esp_i2c_log) fprintf(stderr, "[i2c] %s write %02x\n", b->dev == DEV_MCP ? "mcp" : "dac", b->byte);
            if (b->dev == DEV_MCP) mcp_write(b, b->byte);
            else dac_write(b, b->byte);
            b->slave_low = true;
            b->state = ST_ACK_OUT;
        }
        break;
    case ST_ACK_OUT:                    /* the ACK clock is over */
        b->slave_low = false;
        if (b->read) start_byte_out(b, buttons);
        else { b->state = ST_WRITE; b->bit = 0; b->byte = 0; }
        break;
    case ST_READ:
        if (++b->bit < 8) b->slave_low = !((b->byte << b->bit) & 0x80);
        else { b->slave_low = false; b->state = ST_READ_ACK; }
        break;
    case ST_READ_ACK:                   /* the master's ACK clock is over */
        if (b->master_ack) start_byte_out(b, buttons);
        else b->state = ST_IGNORE;
        break;
    }
}

static void scl_rising(I2cBus *b)
{
    switch (b->state) {
    case ST_ADDR: case ST_WRITE:
        if (b->bit < 8) { b->byte = (uint8_t)(b->byte << 1 | b->sda); b->bit++; }
        break;
    case ST_READ_ACK:
        b->master_ack = !b->sda;
        break;
    }
}

/* The ESP changed what it does to the lines: msda is whether it lets SDA go
   (the slave may still pull it low), scl the SCL level */
void i2c_lines(Esp *s, bool msda, bool scl)
{
    I2cBus *b = &s->i2c;
    const uint8_t keys = s->buttons;
    if (b->scl && !scl) {
        b->scl = false;
        scl_falling(b, keys);
    }
    const bool sda = msda && !b->slave_low;
    if (sda != b->sda) {
        b->sda = sda;
        if (b->scl && scl) {
            if (!sda) {                 /* START (or repeated START) */
                b->state = ST_ADDR;
                b->bit = 0;
                b->byte = 0;
                b->slave_low = false;
            } else {                    /* STOP */
                b->state = ST_IDLE;
                b->slave_low = false;
                b->dev = DEV_NONE;
            }
            b->sda = msda && !b->slave_low;
        }
    }
    if (!b->scl && scl) {
        b->scl = true;
        scl_rising(b);
    }
    b->sda = msda && !b->slave_low;
}
