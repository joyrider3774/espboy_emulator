/*
 * The ESP8266's Xtensa LX106 core.
 *
 * Its configuration (core-isa.h of the lx106 toolchain): 16 plain address
 * registers (no windows), the code density option, MUL16 and MULL (no
 * divide, no MULUH/MULSH), NSA/NSAU, ADDX/SUBX, ABS, extended L32R; no
 * loops, MIN/MAX, SEXT, CLAMPS, booleans, FPU, MAC16 or S32C1I. Anything it
 * lacks raises IllegalInstruction, as on the chip.
 *
 * Exceptions (XEA2, EXCM level 1): user vector VECBASE+0x50, kernel +0x30,
 * double +0x70. Interrupts 0-13 are level 1 (taken through the user/kernel
 * vector with EXCCAUSE 4), 14 is the NMI at level 3 (vector +0x20, EPC3 /
 * EPS3, RFI 3). Debug is level 2 at +0x10. Line 6 is CCOMPARE0, 7 the
 * software interrupt, 8-13 edge triggered, 0-5 level triggered (held by the
 * peripherals in s->irq_level).
 *
 * Instructions are decoded once (esp_decode_page) and executed from the
 * decoded form. The decoder knows the whole Xtensa set; the switch below
 * picks what the LX106 has.
 *
 * Timing: one cycle an instruction, two for loads, three for a taken branch
 * or jump. To be calibrated against the attached ESPboy.
 */
#include <stdio.h>
#include <string.h>
#include "esp8266.h"

#define AR(i) c->ar[i]

enum {
    EXC_ILLEGAL = 0, EXC_SYSCALL = 1, EXC_FETCH_ERROR = 2, EXC_LOAD_STORE_ERROR = 3,
    EXC_LEVEL1_INT = 4, EXC_ALLOCA = 5, EXC_DIVIDE_BY_ZERO = 6, EXC_PRIVILEGED = 8,
    EXC_UNALIGNED = 9,
};

#define VEC_DEBUG   0x10
#define VEC_NMI     0x20
#define VEC_KERNEL  0x30
#define VEC_USER    0x50
#define VEC_DOUBLE  0x70

/* edge, timer, software and NMI lines: INTERRUPT keeps these until cleared */
#define INT_LATCHED 0x7fc0u
/* what INTCLEAR can clear: the software line and the edge lines (the timer
   clears by writing CCOMPARE0) */
#define INT_CLEARABLE ((1u << 7) | 0x3f00u)
#define INT_NMI (1u << INUM_NMI)

int32_t xt_b4const(unsigned r);
int32_t xt_b4constu(unsigned r);

uint32_t xt_ccount(const XtCore *c)
{
    return c->ccount_base + (uint32_t)((c->cycles - c->ccount_ticks) / c->tick_mult);
}

/* The CPU clock changed: CCOUNT carries on from where it is */
void xt_set_tick_mult(XtCore *c, uint32_t mult)
{
    c->ccount_base = xt_ccount(c);
    c->ccount_ticks = c->cycles;
    c->tick_mult = mult ? mult : 1;
}

/* The reset state (XCHAL_HAVE_FULL_RESET): PS.INTLEVEL 15, EXCM, VECBASE at
   the ROM, the reset vector at 0x40000080. 'cycles' goes on: time does not
   restart at a reset */
void xt_reset(XtCore *c)
{
    const uint64_t cycles = c->cycles;
    memset(c, 0, sizeof(*c));
    c->pc = 0x40000080u;
    c->vecbase = 0x40000000u;
    c->ps = 0x1f;
    c->tick_mult = ESP_TICK_HZ / 80000000u;
    c->cycles = cycles;
    c->ccount_ticks = cycles;
    c->loaded = 16;
}

/* ------------------------------------------------------------------------ */
/* Exceptions and interrupts                                                 */
/* ------------------------------------------------------------------------ */

static uint32_t exception(XtCore *c, uint32_t pc, unsigned cause)
{
    c->exccause = cause;
    if (c->ps & PS_EXCM) {
        c->depc = pc;
        return c->vecbase + VEC_DOUBLE;
    }
    c->epc[1] = pc;
    const uint32_t vec = c->vecbase + ((c->ps & PS_UM) ? VEC_USER : VEC_KERNEL);
    c->ps |= PS_EXCM;
    return vec;
}

/* The interrupt to take now, or 0: 3 for the NMI, 1 for a level-1 line */
static inline unsigned pending_level(const XtCore *c)
{
    if ((c->interrupt & INT_NMI) && !c->in_nmi)
        return 3;
    if (c->interrupt & c->intenable & 0x3fffu) {
        unsigned mask = c->ps & PS_INTLEVEL;
        if (c->ps & PS_EXCM) mask = mask < 1 ? 1 : mask;
        if (mask < 1) return 1;
    }
    return 0;
}

static uint32_t take_interrupt(XtCore *c, uint32_t pc, unsigned level)
{
    c->waiting = false;
    if (level == 3) {
        c->interrupt &= ~INT_NMI;       /* edge: taken once */
        c->in_nmi = true;
        c->epc[3] = pc;
        c->eps[3] = c->ps;
        c->ps = (c->ps & ~PS_INTLEVEL) | 3 | PS_EXCM;
        return c->vecbase + VEC_NMI;
    }
    c->exccause = EXC_LEVEL1_INT;
    c->epc[1] = pc;
    const uint32_t vec = c->vecbase + ((c->ps & PS_UM) ? VEC_USER : VEC_KERNEL);
    c->ps |= PS_EXCM;
    return vec;
}

/* A level line or a latched bit changed: INTERRUPT is rebuilt, and the run
   loop stops so the next instruction boundary looks at it */
void xt_irq_changed(Esp *s)
{
    XtCore *c = &s->core;
    c->interrupt = (c->interrupt & INT_LATCHED) | (s->irq_level & 0x3fu);
    if (pending_level(c)) {
        c->waiting = false;
        c->stop = true;
    }
}

/* ------------------------------------------------------------------------ */
/* Special registers                                                         */
/* ------------------------------------------------------------------------ */

static uint32_t rsr(XtCore *c, unsigned sr)
{
    switch (sr) {
    case 3: return c->sar;
    case 5: return c->litbase;
    case 96: return c->ibreakenable;
    case 128: return c->ibreaka;
    case 144: return c->dbreaka;
    case 160: return c->dbreakc;
    case 177: case 178: case 179: return c->epc[sr - 176];
    case 192: return c->depc;
    case 194: case 195: return c->eps[sr - 192];
    case 209: case 210: case 211: return c->excsave[sr - 208];
    case 226: return c->interrupt;
    case 228: return c->intenable;
    case 230: return c->ps;
    case 231: return c->vecbase;
    case 232: return c->exccause;
    case 233: return c->debugcause;
    case 234: return xt_ccount(c);
    case 235: return c->prid;
    case 236: return c->icount;
    case 237: return c->icountlevel;
    case 238: return c->excvaddr;
    case 240: return c->ccompare;
    }
    return 0;
}

static void wsr(Esp *s, XtCore *c, unsigned sr, uint32_t v)
{
    switch (sr) {
    case 3: c->sar = v & 63; break;
    case 5: c->litbase = v & 0xfffff001u; break;
    case 96: c->ibreakenable = v & 1; break;
    case 128: c->ibreaka = v; break;
    case 144: c->dbreaka = v; break;
    case 160: c->dbreakc = v; break;
    case 177: case 178: case 179: c->epc[sr - 176] = v; break;
    case 192: c->depc = v; break;
    case 194: case 195: c->eps[sr - 192] = v; break;
    case 209: case 210: case 211: c->excsave[sr - 208] = v; break;
    case 226: c->interrupt |= v & (1u << INUM_SOFT); break;      /* INTSET */
    case 227: c->interrupt &= ~(v & INT_CLEARABLE); break;        /* INTCLEAR */
    case 228: c->intenable = v & 0x7fffu; break;
    case 230: c->ps = v & 0x7ffffu; break;
    case 231: c->vecbase = v & ~0x3ffu; break;
    case 232: c->exccause = v; break;
    case 233: c->debugcause = v; break;
    case 234: c->ccount_base = v; c->ccount_ticks = c->cycles; break;
    case 236: c->icount = v; break;
    case 237: c->icountlevel = v; break;
    case 238: c->excvaddr = v; break;
    case 240:
        c->ccompare = v;
        c->interrupt &= ~(1u << INUM_TIMER0);
        c->stop = true;         /* the slice was bounded by the old compare value */
        break;
    }
    xt_irq_changed(s);
}

/* ------------------------------------------------------------------------ */
/* The run loop                                                              */
/* ------------------------------------------------------------------------ */

/* The address registers an instruction reads, for the load-use interlock */
static uint16_t read_mask(const XtInsn *d)
{
    const uint16_t s = (uint16_t)(1u << d->s), t = (uint16_t)(1u << d->t), r = (uint16_t)(1u << d->r);
    switch (d->op) {
    case XT_ADD: case XT_ADD_N: case XT_ADDX2: case XT_ADDX4: case XT_ADDX8: case XT_SUB: case XT_SUBX2:
    case XT_SUBX4: case XT_SUBX8: case XT_AND: case XT_OR: case XT_XOR: case XT_MULL: case XT_MUL16U:
    case XT_MUL16S: case XT_SRC: case XT_BEQ: case XT_BNE: case XT_BLT: case XT_BGE: case XT_BLTU:
    case XT_BGEU: case XT_BANY: case XT_BNONE: case XT_BALL: case XT_BNALL: case XT_BBC: case XT_BBS:
    case XT_S8I: case XT_S16I: case XT_S32I: case XT_S32I_N:
        return s | t;
    case XT_MOVEQZ: case XT_MOVNEZ: case XT_MOVLTZ: case XT_MOVGEZ:
        return s | t | r;
    case XT_NEG: case XT_ABS: case XT_SRLI: case XT_SRAI: case XT_EXTUI: case XT_SRL: case XT_SRA:
    case XT_WSR: case XT_XSR:
        return t;
    case XT_SLLI: case XT_SLL: case XT_SSR: case XT_SSL: case XT_SSA8L: case XT_SSA8B: case XT_NSA:
    case XT_NSAU: case XT_MOV_N: case XT_ADDI: case XT_ADDMI: case XT_ADDI_N: case XT_L8UI: case XT_L16UI:
    case XT_L16SI: case XT_L32I: case XT_L32I_N: case XT_BEQZ: case XT_BEQZ_N: case XT_BNEZ: case XT_BNEZ_N:
    case XT_BLTZ: case XT_BGEZ: case XT_BEQI: case XT_BNEI: case XT_BLTI: case XT_BGEI: case XT_BLTUI:
    case XT_BGEUI: case XT_BBCI: case XT_BBSI: case XT_JX: case XT_CALLX0:
        return s;
    case XT_RET: case XT_RET_N:
        return 1;
    }
    return 0;
}

static inline XtInsn *fetch(Esp *s, XtCore *c, uint32_t pc)
{
    XtInsn *page = s->dpage[pc >> 16];
    if (page) {
        XtInsn *d = &page[pc & 0xffff];
        if (d->len) return d;
    } else {
        page = esp_decode_page(s, pc);
        if (!page) return NULL;
    }
    XtInsn *d = &page[pc & 0xffff];
    const uint32_t w = mem_read(s, pc, 1) | mem_read(s, pc + 1, 1) << 8 | mem_read(s, pc + 2, 1) << 16;
    xt_decode(w, d);
    d->regs = read_mask(d);
    /* IRAM code is remembered so that stores into it drop the decoded form */
    if (pc - 0x40100000u < ESP_IRAM_SIZE) {
        const uint32_t off = pc - 0x40100000u;
        s->iram_code[off >> 6] = 1;
        s->iram_code[((off + d->len - 1) & 0xffff) >> 6] = 1;
    }
    return d;
}

/* Debugging: with xt_trace on, the last TRACE_N instructions are kept and
   printed, disassembled, at the first fault */
bool xt_trace = false;
#define TRACE_N 64
static struct { uint32_t pc, a[16]; XtInsn d; } trace_ring[TRACE_N];
static unsigned trace_pos;
static bool trace_dumped;

void xt_trace_dump(void)
{
    if (trace_dumped) return;
    trace_dumped = true;
    fprintf(stderr, "--- last %d instructions ---\n", TRACE_N);
    for (unsigned i = 0; i < TRACE_N; i++) {
        const unsigned k = (trace_pos + i) % TRACE_N;
        if (!trace_ring[k].pc) continue;
        char text[96];
        xt_disasm(&trace_ring[k].d, trace_ring[k].pc, text, sizeof text);
        fprintf(stderr, "%08x  %-32s a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x"
                " a8=%08x a9=%08x a10=%08x a12=%08x a13=%08x a14=%08x a15=%08x\n",
                trace_ring[k].pc, text, trace_ring[k].a[0], trace_ring[k].a[1], trace_ring[k].a[2],
                trace_ring[k].a[3], trace_ring[k].a[4], trace_ring[k].a[5], trace_ring[k].a[6],
                trace_ring[k].a[7], trace_ring[k].a[8], trace_ring[k].a[9], trace_ring[k].a[10],
                trace_ring[k].a[12], trace_ring[k].a[13], trace_ring[k].a[14], trace_ring[k].a[15]);
    }
}

#define BRANCH(cond) do { if (cond) { npc = pc + 4 + (uint32_t)d->imm; cost += 2; } } while (0)
#define JUMP(to) do { npc = (to); cost += 2; } while (0)
#define RAISE(cause) do { npc = exception(c, pc, (cause)); cost += 2; } while (0)

/* Runs until the core's clock reaches 'until', or until something asks the
   loop to stop (an interrupt became pending, a timer was reprogrammed, a
   reset was requested) */
void xt_run(Esp *s, XtCore *c, uint64_t until)
{
    uint32_t pc = c->pc;
    c->stop = false;

    while (c->cycles < until) {
        /* interrupts are looked at between instructions */
        if (c->interrupt & (c->intenable | INT_NMI)) {
            const unsigned lvl = pending_level(c);
            if (lvl) pc = take_interrupt(c, pc, lvl);
        }
        if (c->waiting) {
            c->cycles = until;
            break;
        }

        const XtInsn *d = fetch(s, c, pc);
        if (!d) {
            if (xt_trace) { fprintf(stderr, "fetch fault at %08x\n", pc); xt_trace_dump(); }
            c->excvaddr = pc;
            pc = exception(c, pc, EXC_FETCH_ERROR);
            c->fault_count++;
            c->cycles += c->tick_mult;
            continue;
        }
        if (xt_trace) {
            trace_ring[trace_pos].pc = pc;
            trace_ring[trace_pos].d = *d;
            memcpy(trace_ring[trace_pos].a, c->ar, sizeof c->ar);
            trace_pos = (trace_pos + 1) % TRACE_N;
        }

#ifdef ESP_RWATCH
        /* debugging builds: a function's calls (a2 in, caller) and returns (a2 out) */
        {
            extern uint32_t esp_bp, esp_bp_peek, esp_bp2;
            static uint32_t ret_bp[64];
            static int nret;
            if (pc == esp_bp2)
                fprintf(stderr, "[bp2] %.6f %08x a2=%08x from %08x\n", (double)c->cycles / ESP_TICK_HZ, pc, AR(2), AR(0));
            if (pc == esp_bp && nret < 64) {
                fprintf(stderr, "[bp] %.6f call %08x a2=%08x a3=%08x from %08x peek %08x stack %08x %08x %08x %08x\n",
                        (double)c->cycles / ESP_TICK_HZ, pc, AR(2), AR(3), AR(0),
                        esp_bp_peek ? mem_read(s, esp_bp_peek, 4) : 0,
                        mem_read(s, AR(1) + 12, 4), mem_read(s, AR(1) + 28, 4),
                        mem_read(s, AR(1) + 44, 4), mem_read(s, AR(1) + 60, 4));
                ret_bp[nret++] = AR(0);
            }
            for (int k = nret - 1; k >= 0; k--)
                if (pc == ret_bp[k]) {
                    fprintf(stderr, "[bp]   returns %08x to %08x\n", AR(2), pc);
                    ret_bp[k] = ret_bp[--nret];
                    break;
                }
        }
#endif
        uint32_t npc = pc + d->len;
        /* cycles, plus ticks the memory it reached adds; the load-use interlock */
        unsigned cost = 1 + ((d->regs >> c->loaded) & 1);
        unsigned extra = 0, loaded = 16;
        bool stored = false;
        c->pc = pc;     /* for the peripherals' and the debugger's benefit */
        const unsigned r = d->r, sreg = d->s, t = d->t;

        switch (d->op) {
        /* --- arithmetic --- */
        case XT_ADD: case XT_ADD_N: AR(r) = AR(sreg) + AR(t); break;
        case XT_ADDX2: AR(r) = (AR(sreg) << 1) + AR(t); break;
        case XT_ADDX4: AR(r) = (AR(sreg) << 2) + AR(t); break;
        case XT_ADDX8: AR(r) = (AR(sreg) << 3) + AR(t); break;
        case XT_SUB: AR(r) = AR(sreg) - AR(t); break;
        case XT_SUBX2: AR(r) = (AR(sreg) << 1) - AR(t); break;
        case XT_SUBX4: AR(r) = (AR(sreg) << 2) - AR(t); break;
        case XT_SUBX8: AR(r) = (AR(sreg) << 3) - AR(t); break;
        case XT_AND: AR(r) = AR(sreg) & AR(t); break;
        case XT_OR: AR(r) = AR(sreg) | AR(t); break;
        case XT_XOR: AR(r) = AR(sreg) ^ AR(t); break;
        case XT_NEG: AR(r) = 0u - AR(t); break;
        case XT_ABS: { const int32_t v = (int32_t)AR(t); AR(r) = v < 0 ? 0u - (uint32_t)v : (uint32_t)v; break; }
        case XT_ADDI: case XT_ADDMI: AR(t) = AR(sreg) + (uint32_t)d->imm; break;
        case XT_ADDI_N: AR(r) = AR(sreg) + (uint32_t)d->imm; break;
        case XT_MOVI: AR(t) = (uint32_t)d->imm; break;
        case XT_MOVI_N: AR(sreg) = (uint32_t)d->imm; break;
        case XT_MOV_N: AR(t) = AR(sreg); break;
        case XT_MOVEQZ: if (AR(t) == 0) AR(r) = AR(sreg); break;
        case XT_MOVNEZ: if (AR(t) != 0) AR(r) = AR(sreg); break;
        case XT_MOVLTZ: if ((int32_t)AR(t) < 0) AR(r) = AR(sreg); break;
        case XT_MOVGEZ: if ((int32_t)AR(t) >= 0) AR(r) = AR(sreg); break;
        case XT_NSA: {
            const int32_t v = (int32_t)AR(sreg);
            const uint32_t x = (uint32_t)(v < 0 ? ~v : v);
            AR(t) = x ? (uint32_t)__builtin_clz(x) - 1 : 31;
            break;
        }
        case XT_NSAU: { const uint32_t v = AR(sreg); AR(t) = v ? (uint32_t)__builtin_clz(v) : 32; break; }

        /* --- shifts --- */
        case XT_SLLI: AR(r) = d->imm >= 32 ? 0 : AR(sreg) << d->imm; break;
        case XT_SRLI: AR(r) = AR(t) >> d->imm; break;
        case XT_SRAI: AR(r) = (uint32_t)((int32_t)AR(t) >> d->imm); break;
        case XT_EXTUI: {
            const unsigned sh = (unsigned)(d->imm & 0xff), bits = (unsigned)(d->imm >> 8);
            AR(r) = (AR(t) >> sh) & (bits >= 32 ? 0xffffffffu : ((1u << bits) - 1));
            break;
        }
        case XT_SSR: c->sar = AR(sreg) & 31; break;
        case XT_SSL: c->sar = 32 - (AR(sreg) & 31); break;
        case XT_SSA8L: c->sar = (AR(sreg) & 3) * 8; break;
        case XT_SSA8B: c->sar = 32 - (AR(sreg) & 3) * 8; break;
        case XT_SSAI: c->sar = (uint32_t)d->imm; break;
        case XT_SRC: {
            const uint64_t v = ((uint64_t)AR(sreg) << 32) | AR(t);
            AR(r) = (uint32_t)(v >> (c->sar & 63));
            break;
        }
        case XT_SRL: AR(r) = c->sar >= 32 ? 0 : AR(t) >> c->sar; break;
        case XT_SLL: { const unsigned sh = 32 - (c->sar & 63); AR(r) = sh >= 32 ? 0 : AR(sreg) << sh; break; }
        case XT_SRA: AR(r) = (uint32_t)((int32_t)AR(t) >> (c->sar > 31 ? 31 : c->sar)); break;

        /* --- multiply --- */
        case XT_MUL16U: AR(r) = (AR(sreg) & 0xffff) * (AR(t) & 0xffff); break;
        case XT_MUL16S: AR(r) = (uint32_t)((int16_t)AR(sreg) * (int16_t)AR(t)); break;
        case XT_MULL: AR(r) = AR(sreg) * AR(t); cost = 2; break;

        /* --- loads and stores --- */
        /* a load costs what the memory it reads costs (s->rcost, measured:
           DRAM 1 cycle, IRAM/ROM/flash 6, peripherals 16 at 160 MHz), and an
           instruction using its result right after waits a cycle */
        case XT_L32R: {
            uint32_t a;
            if (c->litbase & 1)     /* extended L32R: from LITBASE, always backwards */
                a = (c->litbase & 0xfffff000u) + (((uint32_t)d->imm | 0xffff0000u) << 2);
            else
                a = xt_target(d, pc);
            AR(t) = mem_read(s, a, 4);
            extra = s->rcost[a >> 16];
            loaded = t;
            break;
        }
        case XT_L8UI: {
            const uint32_t a = AR(sreg) + (uint32_t)d->imm;
            AR(t) = mem_read(s, a, 1);
            extra = s->rcost[a >> 16];
            loaded = t;
            break;
        }
        case XT_L16UI: {
            const uint32_t a = AR(sreg) + (uint32_t)d->imm;
            AR(t) = mem_read(s, a, 2);
            extra = s->rcost[a >> 16];
            loaded = t;
            break;
        }
        case XT_L16SI: {
            const uint32_t a = AR(sreg) + (uint32_t)d->imm;
            AR(t) = (uint32_t)(int16_t)mem_read(s, a, 2);
            extra = s->rcost[a >> 16];
            loaded = t;
            break;
        }
        case XT_L32I: case XT_L32I_N: {
            const uint32_t a = AR(sreg) + (uint32_t)d->imm;
            AR(t) = mem_read(s, a, 4);
            extra = s->rcost[a >> 16];
            loaded = t;
            break;
        }
        case XT_S8I: { const uint32_t a = AR(sreg) + (uint32_t)d->imm; mem_write(s, a, AR(t), 1); extra = s->wcost[a >> 16]; stored = true; break; }
        case XT_S16I: { const uint32_t a = AR(sreg) + (uint32_t)d->imm; mem_write(s, a, AR(t), 2); extra = s->wcost[a >> 16]; stored = true; break; }
        case XT_S32I: case XT_S32I_N: { const uint32_t a = AR(sreg) + (uint32_t)d->imm; mem_write(s, a, AR(t), 4); extra = s->wcost[a >> 16]; stored = true; break; }
        case XT_MEMW: if (c->after_store) cost = 4; break;     /* waits for the store to finish */

        /* --- branches --- */
        case XT_J: JUMP(xt_target(d, pc)); break;
        case XT_JX: JUMP(AR(sreg)); break;
        case XT_BEQZ: case XT_BEQZ_N: BRANCH(AR(sreg) == 0); break;
        case XT_BNEZ: case XT_BNEZ_N: BRANCH(AR(sreg) != 0); break;
        case XT_BLTZ: BRANCH((int32_t)AR(sreg) < 0); break;
        case XT_BGEZ: BRANCH((int32_t)AR(sreg) >= 0); break;
        case XT_BEQI: BRANCH((int32_t)AR(sreg) == xt_b4const(r)); break;
        case XT_BNEI: BRANCH((int32_t)AR(sreg) != xt_b4const(r)); break;
        case XT_BLTI: BRANCH((int32_t)AR(sreg) < xt_b4const(r)); break;
        case XT_BGEI: BRANCH((int32_t)AR(sreg) >= xt_b4const(r)); break;
        case XT_BLTUI: BRANCH(AR(sreg) < (uint32_t)xt_b4constu(r)); break;
        case XT_BGEUI: BRANCH(AR(sreg) >= (uint32_t)xt_b4constu(r)); break;
        case XT_BEQ: BRANCH(AR(sreg) == AR(t)); break;
        case XT_BNE: BRANCH(AR(sreg) != AR(t)); break;
        case XT_BLT: BRANCH((int32_t)AR(sreg) < (int32_t)AR(t)); break;
        case XT_BGE: BRANCH((int32_t)AR(sreg) >= (int32_t)AR(t)); break;
        case XT_BLTU: BRANCH(AR(sreg) < AR(t)); break;
        case XT_BGEU: BRANCH(AR(sreg) >= AR(t)); break;
        case XT_BANY: BRANCH((AR(sreg) & AR(t)) != 0); break;
        case XT_BNONE: BRANCH((AR(sreg) & AR(t)) == 0); break;
        case XT_BALL: BRANCH((~AR(sreg) & AR(t)) == 0); break;
        case XT_BNALL: BRANCH((~AR(sreg) & AR(t)) != 0); break;
        case XT_BBC: BRANCH(!(AR(sreg) >> (AR(t) & 31) & 1)); break;
        case XT_BBS: BRANCH(AR(sreg) >> (AR(t) & 31) & 1); break;
        case XT_BBCI: BRANCH(!(AR(sreg) >> (t | ((r & 1) << 4)) & 1)); break;
        case XT_BBSI: BRANCH(AR(sreg) >> (t | ((r & 1) << 4)) & 1); break;

        /* --- calls and returns (CALL0 ABI only) --- */
        case XT_CALL0: AR(0) = npc; JUMP(xt_target(d, pc)); break;
        case XT_CALLX0: { const uint32_t tgt = AR(sreg); AR(0) = npc; JUMP(tgt); break; }
        case XT_RET: case XT_RET_N: JUMP(AR(0)); cost++; break;    /* 4 cycles: call0 + ret measured 10 with a jump */

        /* --- exception returns --- */
        case XT_RFE: c->ps &= ~PS_EXCM; JUMP(c->epc[1]); xt_irq_changed(s); break;
        case XT_RFUE: c->ps &= ~PS_EXCM; JUMP(c->epc[1]); break;
        case XT_RFDE: JUMP(c->depc); break;
        case XT_RFI:
            if (sreg == 2 || sreg == 3) {
                if (sreg == 3) c->in_nmi = false;
                c->ps = c->eps[sreg];
                JUMP(c->epc[sreg]);
                xt_irq_changed(s);
            } else {
                RAISE(EXC_ILLEGAL);
            }
            break;
        case XT_RSIL: {
            const uint32_t old = c->ps;
            c->ps = (c->ps & ~PS_INTLEVEL) | sreg;
            AR(t) = old;
            xt_irq_changed(s);
            break;
        }
        case XT_WAITI:
            c->ps = (c->ps & ~PS_INTLEVEL) | sreg;
            c->waiting = true;
            xt_irq_changed(s);
            break;

        /* --- traps --- */
        case XT_SYSCALL: RAISE(EXC_SYSCALL); break;
        case XT_BREAK: case XT_BREAK_N:
            /* no debugger: the debug exception, as the panic handler expects */
            c->debugcause = 0x08;
            c->epc[2] = pc;
            c->eps[2] = c->ps;
            c->ps = (c->ps & ~PS_INTLEVEL) | 2 | PS_EXCM;
            JUMP(c->vecbase + VEC_DEBUG);
            break;

        /* --- special registers --- */
        case XT_RSR: AR(t) = rsr(c, (unsigned)d->imm); break;
        case XT_WSR: wsr(s, c, (unsigned)d->imm, AR(t)); break;
        case XT_XSR: {
            const uint32_t old = rsr(c, (unsigned)d->imm);
            wsr(s, c, (unsigned)d->imm, AR(t));
            AR(t) = old;
            break;
        }
        case XT_RER: AR(t) = 0; break;
        case XT_WER: break;

        /* --- nothing to do here: syncs, caches, TLB --- */
        case XT_NOP: case XT_NOP_N: case XT_ISYNC: case XT_RSYNC: case XT_ESYNC: case XT_DSYNC:
        case XT_EXCW: case XT_EXTW: case XT_SIMCALL:
        case XT_DPFR: case XT_DPFW: case XT_DPFRO: case XT_DPFWO: case XT_DHWB: case XT_DHWBI:
        case XT_DHI: case XT_DII: case XT_IPF: case XT_IHI: case XT_III:
        case XT_IITLB: case XT_IDTLB: case XT_WITLB: case XT_WDTLB:
            break;
        case XT_RITLB0: case XT_RITLB1: case XT_RDTLB0: case XT_RDTLB1: case XT_PITLB: case XT_PDTLB:
            AR(t) = 0; break;

        /* everything else is not in the LX106 */
        default:
            if (xt_trace) {
                char text[96];
                xt_disasm(d, pc, text, sizeof text);
                fprintf(stderr, "illegal instruction at %08x: %s\n", pc, text);
                xt_trace_dump();
            }
            RAISE(EXC_ILLEGAL);
            c->fault_count++;
            break;
        }
        c->cycles += (uint64_t)cost * c->tick_mult + extra;
        c->loaded = (uint8_t)loaded;
        c->after_store = stored;
        pc = npc;
        if (c->stop) {
            c->stop = false;
            c->pc = pc;
            if (pending_level(c) && !c->waiting) continue;
            break;
        }
    }
    c->pc = pc;
}
