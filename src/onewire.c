/*
 * A DS18B20 temperature sensor on GPIO2 (D4), the pin the ESPboy's
 * expansion header shares with the WS2812 data line; programs lock the LED
 * (MCP23017 B1) while they talk to a sensor there. The ESPboy has no sensor
 * of its own: this one reports a made-up room temperature that drifts
 * between 20 and 24 degrees over two minutes.
 *
 * 1-Wire as the DS18B20 datasheet times it, decoded from the ESP's edges:
 * a low of 480 us is a reset, answered with a presence pulse 15-60 us after
 * the release; in a slot the sensor samples 15 us after the falling edge (a
 * low shorter than that is a 1) and, when it sends, holds the line low for a
 * 0 until 30 us after the edge (the master samples within 15 us). Powered
 * externally (READ POWER SUPPLY answers 1).
 */
#include "esp8266.h"

#define US (ESP_TICK_HZ / 1000000u)

enum { IDLE, ROM_CMD, MATCH, SEARCH, FUNC, WRITE_SP, SEND, BUSY, POWER };

static const uint8_t rom_id[7] = { 0x28, 0x45, 0x53, 0x50, 0x62, 0x6f, 0x79 };

/* the Dallas CRC: x^8 + x^5 + x^4 + 1, LSB first */
static uint8_t crc8(const uint8_t *p, int n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t b = *p++;
        for (int i = 0; i < 8; i++) {
            const uint8_t mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

static uint8_t rom_byte(int i)
{
    return i < 7 ? rom_id[i] : crc8(rom_id, 7);
}

static void send(OneWire *w, const uint8_t *data, int n)
{
    memcpy(w->tx, data, (size_t)n);
    w->tx_len = n;
    w->tx_pos = 0;
    w->state = SEND;
}

void ow_reset(OneWire *w)
{
    memset(w, 0, sizeof *w);
    w->state = IDLE;
    w->e_th = w->th = 0x4B;         /* the factory alarm settings and 12 bits */
    w->e_tl = w->tl = 0x46;
    w->e_config = w->config = 0x7F;
    w->temp = 85 * 16;              /* the power-on value of the temperature register */
    w->tx_bit = true;
}

/* 20-24 degrees, a two-minute triangle, in 1/16 degrees at the resolution set */
static int16_t fake_temperature(uint64_t t, uint8_t config)
{
    const uint64_t period = 120ull * ESP_TICK_HZ;
    const uint64_t p = t % period;
    const uint64_t half = period / 2;
    const uint64_t up = p < half ? p : period - p;     /* 0..half */
    int v = 20 * 16 + (int)(up * 64 / half);
    const int r = (config >> 5) & 3;                    /* 9-12 bits: the low 3-r bits are 0 */
    return (int16_t)(v & ~((1 << (3 - r)) - 1));
}

static void function(Esp *s, OneWire *w, uint8_t cmd)
{
    const uint64_t t = s->core.cycles;
    switch (cmd) {
    case 0x44: {                    /* CONVERT T: 93.75 ms at 9 bits, doubling per bit */
        const int r = (w->config >> 5) & 3;
        w->convert_done = t + ((uint64_t)93750u * US << r);
        w->temp = fake_temperature(t, w->config);
        w->state = BUSY;
        break;
    }
    case 0xBE: {                    /* READ SCRATCHPAD (the temperature once converted) */
        const int16_t v = t >= w->convert_done ? w->temp : 85 * 16;
        uint8_t sp[9] = { (uint8_t)v, (uint8_t)(v >> 8), w->th, w->tl, w->config, 0xFF,
                          (uint8_t)(0x10 - (v & 15)), 0x10, 0 };
        sp[8] = crc8(sp, 8);
        send(w, sp, 9);
        break;
    }
    case 0x4E: w->state = WRITE_SP; w->tx_pos = 0; break;   /* WRITE SCRATCHPAD: TH TL config */
    case 0x48:                      /* COPY SCRATCHPAD */
        w->e_th = w->th; w->e_tl = w->tl; w->e_config = w->config;
        w->convert_done = t + 10000u * US;
        w->state = BUSY;
        break;
    case 0xB8:                      /* RECALL E2 */
        w->th = w->e_th; w->tl = w->e_tl; w->config = w->e_config;
        w->state = POWER;
        break;
    case 0xB4: w->state = POWER; break;     /* READ POWER SUPPLY: external */
    default: w->state = IDLE; break;
    }
}

static void byte_in(Esp *s, OneWire *w, uint8_t b)
{
    switch (w->state) {
    case ROM_CMD:
        switch (b) {
        case 0x33: {                /* READ ROM */
            uint8_t r[8];
            for (int i = 0; i < 8; i++) r[i] = rom_byte(i);
            send(w, r, 8);
            w->cmd = 0x33;
            return;
        }
        case 0x55: w->state = MATCH; w->tx_pos = 0; return;
        case 0xCC: w->state = FUNC; return;                     /* SKIP ROM */
        case 0xF0: w->state = SEARCH; w->search_bit = 0; w->search_phase = 0; return;
        default: w->state = IDLE; return;                       /* ALARM SEARCH: no alarm */
        }
    case MATCH:
        if (b != rom_byte(w->tx_pos)) { w->state = IDLE; return; }
        if (++w->tx_pos == 8) w->state = FUNC;
        return;
    case FUNC:
        function(s, w, b);
        return;
    case WRITE_SP:
        if (w->tx_pos == 0) w->th = b;
        else if (w->tx_pos == 1) w->tl = b;
        else { w->config = (b & 0x60) | 0x1F; w->state = IDLE; }
        w->tx_pos++;
        return;
    }
}

static bool search_rom_bit(const OneWire *w)
{
    return rom_byte(w->search_bit >> 3) >> (w->search_bit & 7) & 1;
}

void ow_line(Esp *s, bool esp_low)
{
    OneWire *w = &s->ow;
    const uint64_t t = s->core.cycles;
    if (esp_low == w->low) return;
    w->low = esp_low;
    if (esp_low) {
        /* a slot starts: what the sensor sends in it */
        w->fall = t;
        switch (w->state) {
        case SEND: w->tx_bit = w->tx[w->tx_pos >> 3] >> (w->tx_pos & 7) & 1; break;
        case BUSY: w->tx_bit = t >= w->convert_done; break;
        case SEARCH:
            w->tx_bit = w->search_phase == 2 ? 1 : search_rom_bit(w) ^ (w->search_phase == 1);
            break;
        default: w->tx_bit = 1; break;
        }
        return;
    }
    const uint64_t low = t - w->fall;
    if (low >= 300u * US) {         /* reset (480 us); presence 30-150 us after it */
        w->presence_from = t + 30u * US;
        w->presence_to = t + 150u * US;
        w->state = ROM_CMD;
        w->rx = w->rx_n = 0;
        return;
    }
    const bool bit = low < 15u * US;
    switch (w->state) {
    case ROM_CMD: case MATCH: case FUNC: case WRITE_SP:
        w->rx = (uint8_t)(w->rx >> 1 | (bit ? 0x80 : 0));
        if (++w->rx_n == 8) {
            w->rx_n = 0;
            byte_in(s, w, w->rx);
        }
        break;
    case SEARCH:
        if (w->search_phase < 2) { w->search_phase++; break; }
        if (bit != search_rom_bit(w)) { w->state = IDLE; break; }
        w->search_phase = 0;
        if (++w->search_bit == 64) w->state = FUNC;
        break;
    case SEND:
        if (++w->tx_pos == w->tx_len * 8)
            w->state = w->cmd == 0x33 ? FUNC : IDLE;
        if (w->state != SEND) w->cmd = 0;
        break;
    }
}

bool ow_low(const Esp *s)
{
    const OneWire *w = &s->ow;
    const uint64_t t = s->core.cycles;
    if (t >= w->presence_from && t < w->presence_to) return true;
    return !w->tx_bit && t - w->fall < 30u * US;
}
