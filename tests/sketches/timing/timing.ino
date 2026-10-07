// Cycle timing of the ESP8266's core and buses, measured with CCOUNT and
// printed on the serial port (115200), one line per test:
//
//   TIMING name cycles_per_instruction*100
//
// Run on the ESPboy (tools/serial_test.py COM3 8) and in the emulator
// (espboy_headless timing.bin 8) and compare: the emulator's cycle model
// (xt_cpu.c, periph.c) is set from these numbers. Each loop runs with
// interrupts off, 8 copies of the measured instructions in a loop of 100
// iterations; the empty loop's time is subtracted. Tests named i_ run from
// IRAM, f_ from flash (through the instruction cache, warmed first).
#include <Arduino.h>

#define N 100

static inline uint32_t ccount(void)
{
    uint32_t c;
    asm volatile("rsr %0, ccount" : "=r"(c));
    return c;
}

#define REP8(x) x x x x x x x x

// one test: 'body' is the asm repeated 8 times per iteration
#define TEST(attr, name, setup, body)                                        \
    static uint32_t attr name(void)                                          \
    {                                                                        \
        uint32_t t0, t1, n = N, a = 0, b = 0;                                \
        setup;                                                               \
        uint32_t ps;                                                         \
        asm volatile("rsil %0, 15" : "=r"(ps));                              \
        t0 = ccount();                                                       \
        asm volatile("1:\n" REP8(body "\n") "addi.n %0, %0, -1\n bnez %0, 1b\n" \
                     : "+r"(n), "+r"(a), "+r"(b) : : "memory", "a0");        \
        t1 = ccount();                                                       \
        asm volatile("wsr %0, ps; rsync" ::"r"(ps));                         \
        return t1 - t0;                                                      \
    }

#define FLASH_ATTR __attribute__((noinline))

static uint32_t dram_word[16] __attribute__((aligned(4)));
static const uint32_t flash_word[16] PROGMEM __attribute__((aligned(4))) = { 1, 2, 3, 4 };
static uint32_t IRAM_ATTR iram_dummy(void) { return 0x12345678; }

#define BOTH(name, setup, body) \
    TEST(IRAM_ATTR, i_##name, setup, body) \
    TEST(FLASH_ATTR, f_##name, setup, body)

BOTH(empty, , "")
BOTH(add, , "add %1, %1, %2")
BOTH(nop_n, , "nop.n")
BOTH(mull, a = 3; b = 5, "mull %1, %1, %2")
BOTH(mul16u, a = 3; b = 5, "mul16u %1, %1, %2")
BOTH(l32i_dram, a = (uint32_t)dram_word, "l32i %2, %1, 0")
BOTH(l32i_use, a = (uint32_t)dram_word, "l32i %2, %1, 0\n add %2, %2, %2")
BOTH(s32i_dram, a = (uint32_t)dram_word, "s32i %2, %1, 0")
BOTH(l8ui_dram, a = (uint32_t)dram_word, "l8ui %2, %1, 0")
BOTH(l16ui_dram, a = (uint32_t)dram_word, "l16ui %2, %1, 0")
BOTH(l32i_iram, a = (uint32_t)&iram_dummy, "l32i %2, %1, 0")
BOTH(l32i_flash, a = (uint32_t)flash_word, "l32i %2, %1, 0")
BOTH(l32i_rom, a = 0x40000100, "l32i %2, %1, 0")
// l32r only reaches back: the literal sits behind a jump (subtract "j")
BOTH(j_l32r, , "j 3f\n .align 4\n 2: .word 0x12345678\n 3: l32r %2, 2b")
BOTH(j, , "j 2f\n .align 4\n 2:")
BOTH(j_odd, , "j 2f\n .align 4\n _nop.n\n 2:")
BOTH(beqz_taken, a = 0, "beqz %1, 2f\n .align 4\n 2:")
BOTH(beqz_taken_odd, a = 0, "beqz %1, 2f\n .align 4\n _nop.n\n 2:")
BOTH(beqz_not, a = 1, "beqz %1, 2f\n 2:")
BOTH(bne_taken, a = 0; b = 1, "bne %1, %2, 2f\n .align 4\n 2:")
BOTH(call0_ret, , "call0 2f\n j 3f\n .align 4\n 2: ret\n .align 4\n 3:")
BOTH(gpio_read, a = 0x60000318, "l32i %2, %1, 0")
BOTH(gpio_write, a = 0x60000304, "s32i %2, %1, 0")
BOTH(spi_w_write, a = 0x60000140, "s32i %2, %1, 0")
BOTH(spi_cmd_read, a = 0x60000100, "l32i %2, %1, 0")
BOTH(dport_read, a = 0x3ff00014, "l32i %2, %1, 0")
BOTH(wdev_read, a = 0x3ff20c00, "l32i %2, %1, 0")
BOTH(rtc_read, a = 0x6000071c, "l32i %2, %1, 0")
BOTH(rsr_ccount, , "rsr %2, ccount")
BOTH(memw, , "memw")
BOTH(s32i_memw, a = (uint32_t)dram_word, "s32i %2, %1, 0\n memw")

typedef uint32_t (*TestFn)(void);
#define T(name) { "i_" #name, i_##name }, { "f_" #name, f_##name }
static const struct { const char *name; TestFn fn; } tests[] = {
    T(empty), T(add), T(nop_n), T(mull), T(mul16u), T(l32i_dram), T(l32i_use), T(s32i_dram),
    T(l8ui_dram), T(l16ui_dram), T(l32i_iram), T(l32i_flash), T(l32i_rom), T(j_l32r), T(j), T(j_odd),
    T(beqz_taken), T(beqz_taken_odd), T(beqz_not), T(bne_taken), T(call0_ret), T(gpio_read),
    T(gpio_write), T(spi_w_write), T(spi_cmd_read), T(dport_read), T(wdev_read), T(rtc_read),
    T(rsr_ccount), T(memw), T(s32i_memw),
};

void setup()
{
    Serial.begin(115200);
    delay(500);
    // GPIO writes go to GPOS with a 0 mask: nothing changes on the pins
    i_empty(); f_empty();
    const uint32_t ei = i_empty(), ef = f_empty();
    Serial.printf("\nTIMING cpu %u MHz\n", (unsigned)ESP.getCpuFreqMHz());
    for (auto &t : tests) {
        t.fn();     // once to warm the cache
        const uint32_t c = t.fn();
        const uint32_t empty = t.name[0] == 'i' ? ei : ef;
        const bool is_empty = t.fn == i_empty || t.fn == f_empty;
        const int32_t per = (int32_t)((c - (is_empty ? 0 : empty)) * 100 / (N * 8));
        Serial.printf("TIMING %s %d\n", t.name, (int)per);
    }
    Serial.println("TIMING done");
    (void)iram_dummy;
}

void loop()
{
    delay(1000);
}
