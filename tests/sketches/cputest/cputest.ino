// Checksums of the LX106's instructions over pseudo-random operands, printed
// on the serial port (115200) as
//
//   CPU name checksum
//
// Run on the ESPboy (tools/serial_test.py COM3 6) and in the emulator
// (espboy_headless cputest.bin 4): every line must be the same. Each test
// runs one instruction (inline asm) on 4096 operand pairs from an xorshift
// generator, edge values mixed in, and folds the results into a checksum.
#include <Arduino.h>

static uint32_t rng_state;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    // every 8th value an edge case
    static const uint32_t edge[8] = { 0, 1, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0xffff, 0x8000, 31 };
    if ((x & 7) == 0) return edge[(x >> 3) & 7];
    return x;
}

static inline uint32_t mix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x01000193u;
    return h ^ (h >> 15);
}

#define N 4096

// two operand register instructions: op rd, rs, rt
#define RRR(name, insn)                                                      \
    static uint32_t t_##name(void)                                 \
    {                                                                        \
        uint32_t h = 0x811c9dc5u;                                            \
        rng_state = 0x12345678u;                                             \
        for (int i = 0; i < N; i++) {                                        \
            uint32_t a = rnd(), b = rnd(), r;                                \
            asm volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));     \
            h = mix(h, r);                                                   \
        }                                                                    \
        return h;                                                            \
    }

// one operand: op rd, rs
#define RR(name, insn)                                                       \
    static uint32_t t_##name(void)                                 \
    {                                                                        \
        uint32_t h = 0x811c9dc5u;                                            \
        rng_state = 0x23456789u;                                             \
        for (int i = 0; i < N; i++) {                                        \
            uint32_t a = rnd(), r;                                           \
            asm volatile(insn " %0, %1" : "=r"(r) : "r"(a));                 \
            h = mix(h, r);                                                   \
        }                                                                    \
        return h;                                                            \
    }

// immediate forms: op rd, rs, imm (template text)
#define RRI(name, text)                                                      \
    static uint32_t t_##name(void)                                 \
    {                                                                        \
        uint32_t h = 0x811c9dc5u;                                            \
        rng_state = 0x3456789au;                                             \
        for (int i = 0; i < N; i++) {                                        \
            uint32_t a = rnd(), r;                                           \
            asm volatile(text : "=r"(r) : "r"(a));                           \
            h = mix(h, r);                                                   \
        }                                                                    \
        return h;                                                            \
    }

// shifts through SAR: set SAR from b, then op rd, rs[, rt]
#define SARX(name, setsar, insn)                                             \
    static uint32_t t_##name(void)                                 \
    {                                                                        \
        uint32_t h = 0x811c9dc5u;                                            \
        rng_state = 0x456789abu;                                             \
        for (int i = 0; i < N; i++) {                                        \
            uint32_t a = rnd(), b = rnd(), c = rnd(), r;                     \
            asm volatile(setsar " %3\n " insn                                 \
                         : "=r"(r) : "r"(a), "r"(c), "r"(b));                \
            h = mix(h, r);                                                   \
        }                                                                    \
        return h;                                                            \
    }

// conditional moves: r starts as c, then op r, a, b
#define MOVC(name, insn)                                                     \
    static uint32_t t_##name(void)                                 \
    {                                                                        \
        uint32_t h = 0x811c9dc5u;                                            \
        rng_state = 0x56789abcu;                                             \
        for (int i = 0; i < N; i++) {                                        \
            uint32_t a = rnd(), b = rnd(), r = rnd();                        \
            asm volatile(insn " %0, %1, %2" : "+r"(r) : "r"(a), "r"(b));     \
            h = mix(h, r);                                                   \
        }                                                                    \
        return h;                                                            \
    }

// branches: 1 if taken
#define BR2(name, insn)                                                      \
    static uint32_t t_##name(void)                                 \
    {                                                                        \
        uint32_t h = 0x811c9dc5u;                                            \
        rng_state = 0x6789abcdu;                                             \
        for (int i = 0; i < N; i++) {                                        \
            uint32_t a = rnd(), b = (i & 3) == 0 ? a : rnd(), r;             \
            if ((i & 15) == 5) b = a & 31;                                   \
            asm volatile("movi %0, 1\n " insn " %1, %2, 1f\n movi %0, 0\n 1:" \
                         : "=&r"(r) : "r"(a), "r"(b));                       \
            h = mix(h, r + i);                                               \
        }                                                                    \
        return h;                                                            \
    }

#define BR1(name, text)                                                      \
    static uint32_t t_##name(void)                                 \
    {                                                                        \
        uint32_t h = 0x811c9dc5u;                                            \
        rng_state = 0x789abcdeu;                                             \
        for (int i = 0; i < N; i++) {                                        \
            uint32_t a = rnd(), r;                                           \
            if ((i & 7) == 3) a = i & 0x3ff;                                 \
            asm volatile("movi %0, 1\n " text "\n movi %0, 0\n 1:"            \
                         : "=&r"(r) : "r"(a));                               \
            h = mix(h, r + i);                                               \
        }                                                                    \
        return h;                                                            \
    }

RRR(add, "add") RRR(sub, "sub") RRR(and, "and") RRR(or, "or") RRR(xor, "xor")
RRR(addx2, "addx2") RRR(addx4, "addx4") RRR(addx8, "addx8")
RRR(subx2, "subx2") RRR(subx4, "subx4") RRR(subx8, "subx8")
RRR(mull, "mull") RRR(mul16u, "mul16u") RRR(mul16s, "mul16s")
RR(neg, "neg") RR(abs, "abs") RR(nsa, "nsa") RR(nsau, "nsau")
RRI(slli1, "slli %0, %1, 1") RRI(slli31, "slli %0, %1, 31") RRI(slli17, "slli %0, %1, 17")
RRI(srli1, "srli %0, %1, 1") RRI(srli15, "srli %0, %1, 15")
RRI(srai1, "srai %0, %1, 1") RRI(srai31, "srai %0, %1, 31") RRI(srai20, "srai %0, %1, 20")
RRI(extui0_1, "extui %0, %1, 0, 1") RRI(extui3_8, "extui %0, %1, 3, 8") RRI(extui16_16, "extui %0, %1, 16, 16")
RRI(extui31_1, "extui %0, %1, 31, 1") RRI(extui5_16, "extui %0, %1, 5, 16")
RRI(addi, "addi %0, %1, -77") RRI(addmi, "addmi %0, %1, 0x7f00") RRI(addin, "addi.n %0, %1, -1")
SARX(srl, "ssr", "srl %0, %1") SARX(sra, "ssr", "sra %0, %1") SARX(sll, "ssl", "sll %0, %1")
SARX(src, "ssr", "src %0, %1, %2") SARX(src8l, "ssa8l", "src %0, %1, %2") SARX(src8b, "ssa8b", "src %0, %1, %2")
SARX(sll8b, "ssa8b", "sll %0, %1") SARX(srl8l, "ssa8l", "srl %0, %1")
MOVC(moveqz, "moveqz") MOVC(movnez, "movnez") MOVC(movltz, "movltz") MOVC(movgez, "movgez")
BR2(beq, "beq") BR2(bne, "bne") BR2(blt, "blt") BR2(bge, "bge") BR2(bltu, "bltu") BR2(bgeu, "bgeu")
BR2(bany, "bany") BR2(bnone, "bnone") BR2(ball, "ball") BR2(bnall, "bnall") BR2(bbc, "bbc") BR2(bbs, "bbs")
BR1(beqz, "beqz %1, 1f") BR1(bnez, "bnez %1, 1f") BR1(bltz, "bltz %1, 1f") BR1(bgez, "bgez %1, 1f")
BR1(beqi, "beqi %1, 12, 1f") BR1(bnei, "bnei %1, -1, 1f") BR1(blti, "blti %1, 256, 1f") BR1(bgei, "bgei %1, 7, 1f")
BR1(bltui, "bltui %1, 32768, 1f") BR1(bgeui, "bgeui %1, 65536, 1f") BR1(bbci, "bbci %1, 5, 1f") BR1(bbsi, "bbsi %1, 31, 1f")

// loads of every width and sign from a buffer at every alignment the ISA allows
static uint8_t buf[64] __attribute__((aligned(4)));
static uint32_t t_loads(void)
{
    uint32_t h = 0x811c9dc5u;
    rng_state = 0x89abcdefu;
    for (int k = 0; k < 64; k++) buf[k] = (uint8_t)rnd();
    for (int i = 0; i < N; i++) {
        uint32_t o = rnd() & 31, r8, r16u, r16s, r32;
        uint8_t *p = buf + o;
        uint8_t *p2 = buf + (o & ~1u);
        uint8_t *p4 = buf + (o & ~3u);
        asm volatile("l8ui %0, %4, 0\n l16ui %1, %5, 0\n l16si %2, %5, 0\n l32i %3, %6, 0"
                     : "=&r"(r8), "=&r"(r16u), "=&r"(r16s), "=&r"(r32) : "r"(p), "r"(p2), "r"(p4));
        h = mix(mix(mix(mix(h, r8), r16u), r16s), r32);
        buf[(i * 7) & 63] ^= (uint8_t)h;
    }
    return h;
}

static uint32_t t_stores(void)
{
    uint32_t h = 0x811c9dc5u;
    rng_state = 0x9abcdef0u;
    for (int k = 0; k < 64; k++) buf[k] = 0;
    for (int i = 0; i < N; i++) {
        uint32_t o = rnd() & 31, v = rnd();
        asm volatile("s8i %0, %1, 0" ::"r"(v), "r"(buf + o) : "memory");
        asm volatile("s16i %0, %1, 2" ::"r"(v >> 3), "r"(buf + (o & ~1u)) : "memory");
        asm volatile("s32i %0, %1, 4" ::"r"(v ^ i), "r"(buf + (o & ~3u)) : "memory");
        h = mix(h, buf[(i * 5) & 63] | buf[(i * 11) & 63] << 8);
    }
    for (int k = 0; k < 64; k += 4) h = mix(h, *(uint32_t *)(buf + k));
    return h;
}

// the C library's software divide and friends, as games use them
static uint32_t t_divide(void)
{
    uint32_t h = 0x811c9dc5u;
    rng_state = 0xabcdef01u;
    for (int i = 0; i < N; i++) {
        uint32_t a = rnd(), b = rnd() | 1;
        int32_t sa = (int32_t)a, sb = (int32_t)b;
        h = mix(h, a / b); h = mix(h, a % b);
        h = mix(h, (uint32_t)(sa / sb)); h = mix(h, (uint32_t)(sa % sb));
        uint64_t p = (uint64_t)a * b;
        h = mix(h, (uint32_t)(p >> 32));
    }
    return h;
}

static uint32_t t_float(void)
{
    uint32_t h = 0x811c9dc5u;
    rng_state = 0xbcdef012u;
    for (int i = 0; i < 512; i++) {
        float a = (float)(int32_t)rnd() / 65536.0f, b = (float)(rnd() & 0xffff) / 256.0f + 0.5f;
        float r = a * b + a / b - sqrtf(fabsf(a));
        uint32_t u;
        memcpy(&u, &r, 4);
        h = mix(h, u);
        double d = (double)a * 1.5 / (double)b;
        uint64_t du;
        memcpy(&du, &d, 8);
        h = mix(mix(h, (uint32_t)du), (uint32_t)(du >> 32));
    }
    return h;
}

typedef uint32_t (*TestFn)(void);
#define T(n) { #n, t_##n }
static const struct { const char *name; TestFn fn; } tests[] = {
    T(add), T(sub), T(and), T(or), T(xor), T(addx2), T(addx4), T(addx8), T(subx2), T(subx4), T(subx8),
    T(mull), T(mul16u), T(mul16s), T(neg), T(abs), T(nsa), T(nsau),
    T(slli1), T(slli31), T(slli17), T(srli1), T(srli15), T(srai1), T(srai31), T(srai20),
    T(extui0_1), T(extui3_8), T(extui16_16), T(extui31_1), T(extui5_16), T(addi), T(addmi), T(addin),
    T(srl), T(sra), T(sll), T(src), T(src8l), T(src8b), T(sll8b), T(srl8l),
    T(moveqz), T(movnez), T(movltz), T(movgez),
    T(beq), T(bne), T(blt), T(bge), T(bltu), T(bgeu), T(bany), T(bnone), T(ball), T(bnall), T(bbc), T(bbs),
    T(beqz), T(bnez), T(bltz), T(bgez), T(beqi), T(bnei), T(blti), T(bgei), T(bltui), T(bgeui), T(bbci), T(bbsi),
    T(loads), T(stores), T(divide), T(float),
};

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println();
    for (auto &t : tests)
        Serial.printf("CPU %s %08x\n", t.name, (unsigned)t.fn());
    Serial.println("CPU done");
}

void loop()
{
    delay(1000);
}
