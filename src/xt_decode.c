/*
 * Xtensa instruction decoding and objdump-style disassembly, see xt_decode.h.
 *
 * 24-bit instructions, little endian:
 *   op0 3:0  t 7:4  s 11:8  r 15:12  op1 19:16  op2 23:20
 *   n 5:4  m 7:6  imm8 23:16  imm12 23:12  imm16 23:8  offset18 23:6
 * 16-bit (density) instructions have op0 8..13 and use op0, t, s, r only.
 */
#include <stdio.h>
#include <string.h>
#include "xt_decode.h"


#define F(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

static int32_t sext(uint32_t v, int bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

static const int32_t b4const[16] = { -1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256 };
static const int32_t b4constu[16] = { 32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256 };

/* which of r, s, t are address registers */
enum { AR_R = 1, AR_S = 2, AR_T = 4 };

static void set(XtInsn *d, int op, int ar)
{
    d->op = (uint16_t)op;
    unsigned m = 0;
    if (ar & AR_R) m |= 1u << d->r;
    if (ar & AR_S) m |= 1u << d->s;
    if (ar & AR_T) m |= 1u << d->t;
    d->regs = (uint16_t)m;
}

static void decode_qrst(uint32_t w, XtInsn *d)
{
    const unsigned t = d->t, s = d->s, r = d->r;
    const unsigned op1 = F(w, 19, 16), op2 = F(w, 23, 20);
    switch (op1) {
    case 0:     /* RST0 */
        switch (op2) {
        case 0:     /* ST0 */
            switch (r) {
            case 0: {   /* SNM0 */
                const unsigned m = t >> 2, n = t & 3;
                if (m == 0 && n == 0) set(d, XT_ILL, 0);
                else if (m == 2) {
                    if (n == 0) set(d, XT_RET, 0);
                    else if (n == 1) set(d, XT_RETW, 0);
                    else if (n == 2) set(d, XT_JX, AR_S);
                } else if (m == 3) {
                    static const int ops[4] = { XT_CALLX0, XT_CALLX4, XT_CALLX8, XT_CALLX12 };
                    set(d, ops[n], AR_S);
                }
                break;
            }
            case 1: set(d, XT_MOVSP, AR_S | AR_T); break;
            case 2:
                switch (t) {
                case 0: set(d, XT_ISYNC, 0); break;
                case 1: set(d, XT_RSYNC, 0); break;
                case 2: set(d, XT_ESYNC, 0); break;
                case 3: set(d, XT_DSYNC, 0); break;
                case 8: set(d, XT_EXCW, 0); break;
                case 12: set(d, XT_MEMW, 0); break;
                case 13: set(d, XT_EXTW, 0); break;
                case 15: set(d, XT_NOP, 0); break;
                }
                break;
            case 3:
                if (t == 0) {
                    switch (s) {
                    case 0: set(d, XT_RFE, 0); break;
                    case 1: set(d, XT_RFUE, 0); break;
                    case 2: set(d, XT_RFDE, 0); break;
                    case 4: set(d, XT_RFWO, 0); break;
                    case 5: set(d, XT_RFWU, 0); break;
                    }
                } else if (t == 1) set(d, XT_RFI, 0);
                else if (t == 14 && s == 0) set(d, XT_RFDO, 0);
                else if (t == 14 && s == 1) set(d, XT_RFDD, 0);
                break;
            case 4: set(d, XT_BREAK, 0); break;
            case 5:
                if (s == 0 && t == 0) set(d, XT_SYSCALL, 0);
                else if (s == 1 && t == 0) set(d, XT_SIMCALL, 0);
                break;
            case 6: set(d, XT_RSIL, AR_T); break;
            case 7: if (t == 0) set(d, XT_WAITI, 0); break;
            case 8: set(d, XT_ANY4, 0); break;
            case 9: set(d, XT_ALL4, 0); break;
            case 10: set(d, XT_ANY8, 0); break;
            case 11: set(d, XT_ALL8, 0); break;
            }
            break;
        case 1: set(d, XT_AND, AR_R | AR_S | AR_T); break;
        case 2: set(d, XT_OR, AR_R | AR_S | AR_T); break;
        case 3: set(d, XT_XOR, AR_R | AR_S | AR_T); break;
        case 4:     /* ST1 */
            switch (r) {
            case 0: set(d, XT_SSR, AR_S); break;
            case 1: set(d, XT_SSL, AR_S); break;
            case 2: set(d, XT_SSA8L, AR_S); break;
            case 3: set(d, XT_SSA8B, AR_S); break;
            case 4: set(d, XT_SSAI, 0); d->imm = (int32_t)(s | ((t & 1) << 4)); break;
            case 6: set(d, XT_RER, AR_S | AR_T); break;
            case 7: set(d, XT_WER, AR_S | AR_T); break;
            case 8: set(d, XT_ROTW, 0); d->imm = sext(t, 4); break;
            case 14: set(d, XT_NSA, AR_S | AR_T); break;
            case 15: set(d, XT_NSAU, AR_S | AR_T); break;
            }
            break;
        case 5:     /* TLB */
            switch (r) {
            case 3: set(d, XT_RITLB0, AR_S | AR_T); break;
            case 4: set(d, XT_IITLB, AR_S); break;
            case 5: set(d, XT_PITLB, AR_S | AR_T); break;
            case 6: set(d, XT_WITLB, AR_S | AR_T); break;
            case 7: set(d, XT_RITLB1, AR_S | AR_T); break;
            case 11: set(d, XT_RDTLB0, AR_S | AR_T); break;
            case 12: set(d, XT_IDTLB, AR_S); break;
            case 13: set(d, XT_PDTLB, AR_S | AR_T); break;
            case 14: set(d, XT_WDTLB, AR_S | AR_T); break;
            case 15: set(d, XT_RDTLB1, AR_S | AR_T); break;
            }
            break;
        case 6:
            if (s == 0) set(d, XT_NEG, AR_R | AR_T);
            else if (s == 1) set(d, XT_ABS, AR_R | AR_T);
            break;
        case 8: set(d, XT_ADD, AR_R | AR_S | AR_T); break;
        case 9: set(d, XT_ADDX2, AR_R | AR_S | AR_T); break;
        case 10: set(d, XT_ADDX4, AR_R | AR_S | AR_T); break;
        case 11: set(d, XT_ADDX8, AR_R | AR_S | AR_T); break;
        case 12: set(d, XT_SUB, AR_R | AR_S | AR_T); break;
        case 13: set(d, XT_SUBX2, AR_R | AR_S | AR_T); break;
        case 14: set(d, XT_SUBX4, AR_R | AR_S | AR_T); break;
        case 15: set(d, XT_SUBX8, AR_R | AR_S | AR_T); break;
        }
        break;

    case 1:     /* RST1 */
        switch (op2) {
        case 0: case 1:
            set(d, XT_SLLI, AR_R | AR_S);
            d->imm = 32 - (int32_t)(((op2 & 1) << 4) | t);
            break;
        case 2: case 3:
            set(d, XT_SRAI, AR_R | AR_T);
            d->imm = (int32_t)(((op2 & 1) << 4) | s);
            break;
        case 4: set(d, XT_SRLI, AR_R | AR_T); d->imm = (int32_t)s; break;
        case 6: set(d, XT_XSR, AR_T); d->imm = (int32_t)((r << 4) | s); break;
        case 8: set(d, XT_SRC, AR_R | AR_S | AR_T); break;
        case 9: if (s == 0) set(d, XT_SRL, AR_R | AR_T); break;
        case 10: if (t == 0) set(d, XT_SLL, AR_R | AR_S); break;
        case 11: if (s == 0) set(d, XT_SRA, AR_R | AR_T); break;
        case 12: set(d, XT_MUL16U, AR_R | AR_S | AR_T); break;
        case 13: set(d, XT_MUL16S, AR_R | AR_S | AR_T); break;
        }
        break;

    case 2:     /* RST2 */
        switch (op2) {
        case 0: set(d, XT_ANDB, 0); break;
        case 1: set(d, XT_ANDBC, 0); break;
        case 2: set(d, XT_ORB, 0); break;
        case 3: set(d, XT_ORBC, 0); break;
        case 4: set(d, XT_XORB, 0); break;
        case 6: set(d, XT_SALTU, AR_R | AR_S | AR_T); break;
        case 7: set(d, XT_SALT, AR_R | AR_S | AR_T); break;
        case 8: set(d, XT_MULL, AR_R | AR_S | AR_T); break;
        case 10: set(d, XT_MULUH, AR_R | AR_S | AR_T); break;
        case 11: set(d, XT_MULSH, AR_R | AR_S | AR_T); break;
        case 12: set(d, XT_QUOU, AR_R | AR_S | AR_T); break;
        case 13: set(d, XT_QUOS, AR_R | AR_S | AR_T); break;
        case 14: set(d, XT_REMU, AR_R | AR_S | AR_T); break;
        case 15: set(d, XT_REMS, AR_R | AR_S | AR_T); break;
        }
        break;

    case 3:     /* RST3 */
        switch (op2) {
        case 0: set(d, XT_RSR, AR_T); d->imm = (int32_t)((r << 4) | s); break;
        case 1: set(d, XT_WSR, AR_T); d->imm = (int32_t)((r << 4) | s); break;
        case 2: set(d, XT_SEXT, AR_R | AR_S); d->imm = (int32_t)t + 7; break;
        case 3: set(d, XT_CLAMPS, AR_R | AR_S); d->imm = (int32_t)t + 7; break;
        case 4: set(d, XT_MIN, AR_R | AR_S | AR_T); break;
        case 5: set(d, XT_MAX, AR_R | AR_S | AR_T); break;
        case 6: set(d, XT_MINU, AR_R | AR_S | AR_T); break;
        case 7: set(d, XT_MAXU, AR_R | AR_S | AR_T); break;
        case 8: set(d, XT_MOVEQZ, AR_R | AR_S | AR_T); break;
        case 9: set(d, XT_MOVNEZ, AR_R | AR_S | AR_T); break;
        case 10: set(d, XT_MOVLTZ, AR_R | AR_S | AR_T); break;
        case 11: set(d, XT_MOVGEZ, AR_R | AR_S | AR_T); break;
        case 12: set(d, XT_MOVF, AR_R | AR_S); break;
        case 13: set(d, XT_MOVT, AR_R | AR_S); break;
        case 14: set(d, XT_RUR, AR_R); d->imm = (int32_t)((s << 4) | t); break;
        case 15: set(d, XT_WUR, AR_T); d->imm = (int32_t)((r << 4) | s); break;
        }
        break;

    case 4: case 5:     /* EXTUI: shift in imm, mask width in imm >> 8 */
        set(d, XT_EXTUI, AR_R | AR_T);
        d->imm = (int32_t)((s | ((op1 & 1) << 4)) | ((op2 + 1) << 8));
        break;

    case 8:     /* LSCX */
        switch (op2) {
        case 0: set(d, XT_LSX, AR_S | AR_T); break;
        case 1: set(d, XT_LSXP, AR_S | AR_T); break;
        case 4: set(d, XT_SSX, AR_S | AR_T); break;
        case 5: set(d, XT_SSXP, AR_S | AR_T); break;
        }
        break;

    case 9:     /* LSC4 */
        if (op2 == 0) { set(d, XT_L32E, AR_S | AR_T); d->imm = (int32_t)(r << 2) - 64; }
        else if (op2 == 4) { set(d, XT_S32E, AR_S | AR_T); d->imm = (int32_t)(r << 2) - 64; }
        else if (op2 == 5) { set(d, XT_S32NB, AR_S | AR_T); d->imm = (int32_t)(r << 2); }
        break;

    case 10:    /* FP0 */
        switch (op2) {
        case 0: set(d, XT_ADD_S, 0); break;
        case 1: set(d, XT_SUB_S, 0); break;
        case 2: set(d, XT_MUL_S, 0); break;
        case 4: set(d, XT_MADD_S, 0); break;
        case 5: set(d, XT_MSUB_S, 0); break;
        case 6: set(d, XT_MADDN_S, 0); break;
        case 7: set(d, XT_DIVN_S, 0); break;
        case 8: set(d, XT_ROUND_S, AR_R); break;
        case 9: set(d, XT_TRUNC_S, AR_R); break;
        case 10: set(d, XT_FLOOR_S, AR_R); break;
        case 11: set(d, XT_CEIL_S, AR_R); break;
        case 12: set(d, XT_FLOAT_S, AR_S); break;
        case 13: set(d, XT_UFLOAT_S, AR_S); break;
        case 14: set(d, XT_UTRUNC_S, AR_R); break;
        case 15:
            switch (t) {
            case 0: set(d, XT_MOV_S, 0); break;
            case 1: set(d, XT_ABS_S, 0); break;
            case 3: set(d, XT_CONST_S, 0); break;
            case 4: set(d, XT_RFR, AR_R); break;
            case 5: set(d, XT_WFR, AR_S); break;
            case 6: set(d, XT_NEG_S, 0); break;
            case 7: set(d, XT_DIV0_S, 0); break;
            case 8: set(d, XT_RECIP0_S, 0); break;
            case 9: set(d, XT_SQRT0_S, 0); break;
            case 10: set(d, XT_RSQRT0_S, 0); break;
            case 11: set(d, XT_NEXP01_S, 0); break;
            case 12: set(d, XT_MKSADJ_S, 0); break;
            case 13: set(d, XT_MKDADJ_S, 0); break;
            case 14: set(d, XT_ADDEXP_S, 0); break;
            case 15: set(d, XT_ADDEXPM_S, 0); break;
            }
            break;
        }
        break;

    case 11:    /* FP1 */
        switch (op2) {
        case 1: set(d, XT_UN_S, 0); break;
        case 2: set(d, XT_OEQ_S, 0); break;
        case 3: set(d, XT_UEQ_S, 0); break;
        case 4: set(d, XT_OLT_S, 0); break;
        case 5: set(d, XT_ULT_S, 0); break;
        case 6: set(d, XT_OLE_S, 0); break;
        case 7: set(d, XT_ULE_S, 0); break;
        case 8: set(d, XT_MOVEQZ_S, AR_T); break;
        case 9: set(d, XT_MOVNEZ_S, AR_T); break;
        case 10: set(d, XT_MOVLTZ_S, AR_T); break;
        case 11: set(d, XT_MOVGEZ_S, AR_T); break;
        case 12: set(d, XT_MOVF_S, 0); break;
        case 13: set(d, XT_MOVT_S, 0); break;
        }
        break;
    }
}

static void decode_lsai(uint32_t w, XtInsn *d)
{
    const unsigned imm8 = F(w, 23, 16);
    switch (d->r) {
    case 0: set(d, XT_L8UI, AR_S | AR_T); d->imm = (int32_t)imm8; break;
    case 1: set(d, XT_L16UI, AR_S | AR_T); d->imm = (int32_t)(imm8 << 1); break;
    case 2: set(d, XT_L32I, AR_S | AR_T); d->imm = (int32_t)(imm8 << 2); break;
    case 4: set(d, XT_S8I, AR_S | AR_T); d->imm = (int32_t)imm8; break;
    case 5: set(d, XT_S16I, AR_S | AR_T); d->imm = (int32_t)(imm8 << 1); break;
    case 6: set(d, XT_S32I, AR_S | AR_T); d->imm = (int32_t)(imm8 << 2); break;
    case 7: {   /* CACHE */
        d->imm = (int32_t)(imm8 << 2);
        switch (d->t) {
        case 0: set(d, XT_DPFR, AR_S); break;
        case 1: set(d, XT_DPFW, AR_S); break;
        case 2: set(d, XT_DPFRO, AR_S); break;
        case 3: set(d, XT_DPFWO, AR_S); break;
        case 4: set(d, XT_DHWB, AR_S); break;
        case 5: set(d, XT_DHWBI, AR_S); break;
        case 6: set(d, XT_DHI, AR_S); break;
        case 7: set(d, XT_DII, AR_S); break;
        case 8: {
            const unsigned op1 = imm8 >> 4;
            d->imm = (int32_t)((imm8 & 15) << 4);
            if (op1 == 0) set(d, XT_DPFL, AR_S);
            else if (op1 == 2) set(d, XT_DHU, AR_S);
            else if (op1 == 3) set(d, XT_DIU, AR_S);
            else if (op1 == 4) set(d, XT_DIWB, AR_S);
            else if (op1 == 5) set(d, XT_DIWBI, AR_S);
            break;
        }
        case 12: set(d, XT_IPF, AR_S); break;
        case 13: {
            const unsigned op1 = imm8 >> 4;
            d->imm = (int32_t)((imm8 & 15) << 4);
            if (op1 == 0) set(d, XT_IPFL, AR_S);
            else if (op1 == 2) set(d, XT_IHU, AR_S);
            else if (op1 == 3) set(d, XT_IIU, AR_S);
            break;
        }
        case 14: set(d, XT_IHI, AR_S); break;
        case 15: set(d, XT_III, AR_S); break;
        }
        break;
    }
    case 9: set(d, XT_L16SI, AR_S | AR_T); d->imm = (int32_t)(imm8 << 1); break;
    case 10: set(d, XT_MOVI, AR_T); d->imm = sext((d->s << 8) | imm8, 12); break;
    case 11: set(d, XT_L32AI, AR_S | AR_T); d->imm = (int32_t)(imm8 << 2); break;
    case 12: set(d, XT_ADDI, AR_S | AR_T); d->imm = sext(imm8, 8); break;
    case 13: set(d, XT_ADDMI, AR_S | AR_T); d->imm = sext(imm8, 8) * 256; break;
    case 14: set(d, XT_S32C1I, AR_S | AR_T); d->imm = (int32_t)(imm8 << 2); break;
    case 15: set(d, XT_S32RI, AR_S | AR_T); d->imm = (int32_t)(imm8 << 2); break;
    }
}

static void decode_si(uint32_t w, XtInsn *d)
{
    const unsigned n = F(w, 5, 4), m = F(w, 7, 6);
    const int32_t imm8 = sext(F(w, 23, 16), 8);
    switch (n) {
    case 0: set(d, XT_J, 0); d->imm = sext(F(w, 23, 6), 18); break;
    case 1: {
        static const int ops[4] = { XT_BEQZ, XT_BNEZ, XT_BLTZ, XT_BGEZ };
        set(d, ops[m], AR_S);
        d->imm = sext(F(w, 23, 12), 12);
        break;
    }
    case 2: {
        static const int ops[4] = { XT_BEQI, XT_BNEI, XT_BLTI, XT_BGEI };
        set(d, ops[m], AR_S);
        d->imm = imm8;
        break;
    }
    case 3:
        switch (m) {
        case 0: set(d, XT_ENTRY, AR_S); d->imm = (int32_t)(F(w, 23, 12) << 3); break;
        case 1:
            switch (d->r) {
            case 0: set(d, XT_BF, 0); d->imm = imm8; break;
            case 1: set(d, XT_BT, 0); d->imm = imm8; break;
            case 8: set(d, XT_LOOP, AR_S); d->imm = (int32_t)F(w, 23, 16); break;
            case 9: set(d, XT_LOOPNEZ, AR_S); d->imm = (int32_t)F(w, 23, 16); break;
            case 10: set(d, XT_LOOPGTZ, AR_S); d->imm = (int32_t)F(w, 23, 16); break;
            }
            break;
        case 2: set(d, XT_BLTUI, AR_S); d->imm = imm8; break;
        case 3: set(d, XT_BGEUI, AR_S); d->imm = imm8; break;
        }
        break;
    }
}

static void decode_b(uint32_t w, XtInsn *d)
{
    d->imm = sext(F(w, 23, 16), 8);
    switch (d->r) {
    case 0: set(d, XT_BNONE, AR_S | AR_T); break;
    case 1: set(d, XT_BEQ, AR_S | AR_T); break;
    case 2: set(d, XT_BLT, AR_S | AR_T); break;
    case 3: set(d, XT_BLTU, AR_S | AR_T); break;
    case 4: set(d, XT_BALL, AR_S | AR_T); break;
    case 5: set(d, XT_BBC, AR_S | AR_T); break;
    case 6: case 7: set(d, XT_BBCI, AR_S); break;
    case 8: set(d, XT_BANY, AR_S | AR_T); break;
    case 9: set(d, XT_BNE, AR_S | AR_T); break;
    case 10: set(d, XT_BGE, AR_S | AR_T); break;
    case 11: set(d, XT_BGEU, AR_S | AR_T); break;
    case 12: set(d, XT_BNALL, AR_S | AR_T); break;
    case 13: set(d, XT_BBS, AR_S | AR_T); break;
    case 14: case 15: set(d, XT_BBSI, AR_S); break;
    }
}

void xt_decode(uint32_t w, XtInsn *d)
{
    memset(d, 0, sizeof(*d));
    const unsigned op0 = w & 15;
    d->t = (uint8_t)F(w, 7, 4);
    d->s = (uint8_t)F(w, 11, 8);
    d->r = (uint8_t)F(w, 15, 12);
    d->len = (op0 >= 8 && op0 <= 13) ? 2 : 3;
    d->raw = d->len == 2 ? (w & 0xffff) : (w & 0xffffff);

    switch (op0) {
    case 0: decode_qrst(w, d); break;
    case 1: set(d, XT_L32R, AR_T); d->imm = (int32_t)F(w, 23, 8); break;
    case 2: decode_lsai(w, d); break;
    case 3: {   /* LSCI */
        const unsigned imm8 = F(w, 23, 16);
        d->imm = (int32_t)(imm8 << 2);
        switch (d->r) {
        case 0: set(d, XT_LSI, AR_S); break;
        case 4: set(d, XT_SSI, AR_S); break;
        case 8: set(d, XT_LSIU, AR_S); break;
        case 12: set(d, XT_SSIU, AR_S); break;
        }
        break;
    }
    case 4: {           /* MAC16 */
        const unsigned op2 = F(w, 23, 20), op1 = F(w, 19, 16);
        if (op2 == 8 || op2 == 9) {
            /* LDINC / LDDEC mw, as: mw is r[1:0] */
            if (op1 == 0 && !(d->r & 12) && !d->t) set(d, op2 == 8 ? XT_LDINC : XT_LDDEC, AR_S);
            break;
        }
        if (op2 > 7) break;
        const unsigned kind = op1 >> 2;
        /* UMUL only between address registers; the load forms are MULA only */
        if (kind == 0 && op2 != XT_MAC_AA) break;
        if ((op2 == XT_MAC_DD_LDINC || op2 == XT_MAC_DD_LDDEC || op2 == XT_MAC_DA_LDINC ||
             op2 == XT_MAC_DA_LDDEC) && kind != 2) break;
        static const int ar[8] = {
            AR_S, AR_S, 0, AR_S, AR_S | AR_T, AR_S | AR_T, AR_T, AR_S | AR_T,
        };
        set(d, XT_MAC16, ar[op2]);
        d->imm = (int32_t)(op2 | kind << 4 | (op1 & 3) << 6);
        break;
    }
    case 5: {
        static const int ops[4] = { XT_CALL0, XT_CALL4, XT_CALL8, XT_CALL12 };
        const unsigned n = F(w, 5, 4);
        set(d, ops[n], 0);
        d->imm = sext(F(w, 23, 6), 18);
        break;
    }
    case 6: decode_si(w, d); break;
    case 7: decode_b(w, d); break;

    case 8: set(d, XT_L32I_N, AR_S | AR_T); d->imm = (int32_t)(d->r << 2); break;
    case 9: set(d, XT_S32I_N, AR_S | AR_T); d->imm = (int32_t)(d->r << 2); break;
    case 10: set(d, XT_ADD_N, AR_R | AR_S | AR_T); break;
    case 11: set(d, XT_ADDI_N, AR_R | AR_S); d->imm = d->t ? (int32_t)d->t : -1; break;
    case 12:
        if (!(d->t & 8)) {
            int v = (int)(((d->t & 7) << 4) | d->r);
            if (v >= 96) v -= 128;
            set(d, XT_MOVI_N, AR_S);
            d->imm = v;
        } else {
            set(d, (d->t & 4) ? XT_BNEZ_N : XT_BEQZ_N, AR_S);
            d->imm = (int32_t)(((d->t & 3) << 4) | d->r);
        }
        break;
    case 13:
        if (d->r == 0) set(d, XT_MOV_N, AR_S | AR_T);
        else if (d->r == 15) {
            switch (d->t) {
            case 0: set(d, XT_RET_N, 0); break;
            case 1: set(d, XT_RETW_N, 0); break;
            case 2: set(d, XT_BREAK_N, 0); break;
            case 3: set(d, XT_NOP_N, 0); break;
            case 6: set(d, XT_ILL_N, 0); break;
            }
        }
        break;
    default: break;     /* op0 14, 15: reserved */
    }
}

uint32_t xt_target(const XtInsn *d, uint32_t pc)
{
    switch (d->op) {
    case XT_CALL0: case XT_CALL4: case XT_CALL8: case XT_CALL12:
        return (pc & ~3u) + ((uint32_t)d->imm << 2) + 4;
    case XT_J:
        return pc + 4 + (uint32_t)d->imm;
    case XT_L32R:
        return ((pc + 3) & ~3u) + ((0xffff0000u | (uint32_t)d->imm) << 2);
    case XT_BEQZ: case XT_BNEZ: case XT_BLTZ: case XT_BGEZ:
    case XT_BEQI: case XT_BNEI: case XT_BLTI: case XT_BGEI: case XT_BLTUI: case XT_BGEUI:
    case XT_BF: case XT_BT: case XT_LOOP: case XT_LOOPNEZ: case XT_LOOPGTZ:
    case XT_BNONE: case XT_BEQ: case XT_BLT: case XT_BLTU: case XT_BALL: case XT_BBC: case XT_BBCI:
    case XT_BANY: case XT_BNE: case XT_BGE: case XT_BGEU: case XT_BNALL: case XT_BBS: case XT_BBSI:
    case XT_BEQZ_N: case XT_BNEZ_N:
        return pc + 4 + (uint32_t)d->imm;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Disassembly                                                               */
/* ------------------------------------------------------------------------ */

const char *xt_sr_name(unsigned sr, int write)
{
    switch (sr) {
    case 0: return "lbeg"; case 1: return "lend"; case 2: return "lcount"; case 3: return "sar";
    case 4: return "br"; case 5: return "litbase"; case 12: return "scompare1";
    case 16: return "acclo"; case 17: return "acchi";
    case 32: return "m0"; case 33: return "m1"; case 34: return "m2"; case 35: return "m3";
    case 72: return "windowbase"; case 73: return "windowstart";
    case 83: return "ptevaddr"; case 89: return "mmid"; case 90: return "rasid";
    case 91: return "itlbcfg"; case 92: return "dtlbcfg";
    case 96: return "ibreakenable"; case 97: return "memctl"; case 98: return "cacheattr";
    case 99: return "atomctl"; case 104: return "ddr";
    case 128: return "ibreaka0"; case 129: return "ibreaka1";
    case 144: return "dbreaka0"; case 145: return "dbreaka1";
    case 160: return "dbreakc0"; case 161: return "dbreakc1";
    case 177: return "epc1"; case 178: return "epc2"; case 179: return "epc3"; case 180: return "epc4";
    case 181: return "epc5"; case 182: return "epc6"; case 183: return "epc7";
    case 192: return "depc";
    case 194: return "eps2"; case 195: return "eps3"; case 196: return "eps4"; case 197: return "eps5";
    case 198: return "eps6"; case 199: return "eps7";
    case 209: return "excsave1"; case 210: return "excsave2"; case 211: return "excsave3";
    case 212: return "excsave4"; case 213: return "excsave5"; case 214: return "excsave6";
    case 215: return "excsave7";
    case 224: return "cpenable";
    case 226: return write ? "intset" : "interrupt";
    case 227: return "intclear"; case 228: return "intenable"; case 230: return "ps";
    case 231: return "vecbase"; case 232: return "exccause"; case 233: return "debugcause";
    case 234: return "ccount"; case 235: return "prid"; case 236: return "icount";
    case 237: return "icountlevel"; case 238: return "excvaddr";
    case 240: return "ccompare0"; case 241: return "ccompare1"; case 242: return "ccompare2";
    case 244: return "misc0"; case 245: return "misc1"; case 246: return "misc2"; case 247: return "misc3";
    }
    return NULL;
}

static const char *ur_name(unsigned ur)
{
    /* 0..18 are the ESP32-S3 PIE extension's state */
    static const char *pie[19] = {
        "accx_0", "accx_1", "qacc_h_0", "qacc_h_1", "qacc_h_2", "qacc_h_3", "qacc_h_4",
        "qacc_l_0", "qacc_l_1", "qacc_l_2", "qacc_l_3", "qacc_l_4", NULL, "sar_byte",
        "fft_bit_width", "ua_state_0", "ua_state_1", "ua_state_2", "ua_state_3",
    };
    if (ur < 19) return pie[ur];
    switch (ur) {
    case 231: return "threadptr"; case 232: return "fcr"; case 233: return "fsr";
    }
    return NULL;
}

/* objdump prints an immediate beyond +-255 in hex, anything else in decimal */
static const char *num(int32_t v, char *buf)
{
    if (v > 255 || v < -255) sprintf(buf, "0x%x", (unsigned)v);
    else sprintf(buf, "%d", v);
    return buf;
}

static const char *const names[XT_OP_COUNT] = {
    [XT_UNKNOWN] = "?", [XT_ILL] = "ill",
    [XT_AND] = "and", [XT_OR] = "or", [XT_XOR] = "xor", [XT_ADD] = "add", [XT_ADDX2] = "addx2",
    [XT_ADDX4] = "addx4", [XT_ADDX8] = "addx8", [XT_SUB] = "sub", [XT_SUBX2] = "subx2",
    [XT_SUBX4] = "subx4", [XT_SUBX8] = "subx8", [XT_NEG] = "neg", [XT_ABS] = "abs",
    [XT_RET] = "ret", [XT_RETW] = "retw", [XT_JX] = "jx", [XT_CALLX0] = "callx0",
    [XT_CALLX4] = "callx4", [XT_CALLX8] = "callx8", [XT_CALLX12] = "callx12",
    [XT_MOVSP] = "movsp", [XT_ISYNC] = "isync", [XT_RSYNC] = "rsync", [XT_ESYNC] = "esync",
    [XT_DSYNC] = "dsync", [XT_EXCW] = "excw", [XT_MEMW] = "memw", [XT_EXTW] = "extw", [XT_NOP] = "nop",
    [XT_RFE] = "rfe", [XT_RFUE] = "rfue", [XT_RFDE] = "rfde", [XT_RFWO] = "rfwo", [XT_RFWU] = "rfwu",
    [XT_RFI] = "rfi", [XT_RFDO] = "rfdo", [XT_RFDD] = "rfdd",
    [XT_BREAK] = "break", [XT_SYSCALL] = "syscall", [XT_SIMCALL] = "simcall", [XT_RSIL] = "rsil",
    [XT_WAITI] = "waiti", [XT_ANY4] = "any4", [XT_ALL4] = "all4", [XT_ANY8] = "any8", [XT_ALL8] = "all8",
    [XT_SSR] = "ssr", [XT_SSL] = "ssl", [XT_SSA8L] = "ssa8l", [XT_SSA8B] = "ssa8b", [XT_SSAI] = "ssai",
    [XT_RER] = "rer", [XT_WER] = "wer", [XT_ROTW] = "rotw", [XT_NSA] = "nsa", [XT_NSAU] = "nsau",
    [XT_RITLB0] = "ritlb0", [XT_IITLB] = "iitlb", [XT_PITLB] = "pitlb", [XT_WITLB] = "witlb",
    [XT_RITLB1] = "ritlb1", [XT_RDTLB0] = "rdtlb0", [XT_IDTLB] = "idtlb", [XT_PDTLB] = "pdtlb",
    [XT_WDTLB] = "wdtlb", [XT_RDTLB1] = "rdtlb1",
    [XT_SLLI] = "slli", [XT_SRAI] = "srai", [XT_SRLI] = "srli", [XT_XSR] = "xsr", [XT_SRC] = "src",
    [XT_SRL] = "srl", [XT_SLL] = "sll", [XT_SRA] = "sra", [XT_MUL16U] = "mul16u", [XT_MUL16S] = "mul16s",
    [XT_ANDB] = "andb", [XT_ANDBC] = "andbc", [XT_ORB] = "orb", [XT_ORBC] = "orbc", [XT_XORB] = "xorb",
    [XT_MULL] = "mull", [XT_MULUH] = "muluh", [XT_MULSH] = "mulsh", [XT_QUOU] = "quou",
    [XT_QUOS] = "quos", [XT_REMU] = "remu", [XT_REMS] = "rems",
    [XT_RSR] = "rsr", [XT_WSR] = "wsr", [XT_SEXT] = "sext", [XT_CLAMPS] = "clamps", [XT_MIN] = "min",
    [XT_MAX] = "max", [XT_MINU] = "minu", [XT_MAXU] = "maxu", [XT_MOVEQZ] = "moveqz",
    [XT_MOVNEZ] = "movnez", [XT_MOVLTZ] = "movltz", [XT_MOVGEZ] = "movgez", [XT_MOVF] = "movf",
    [XT_MOVT] = "movt", [XT_RUR] = "rur", [XT_WUR] = "wur", [XT_EXTUI] = "extui",
    [XT_LSX] = "lsx", [XT_LSXP] = "lsxp", [XT_SSX] = "ssx", [XT_SSXP] = "ssxp",
    [XT_L32E] = "l32e", [XT_S32E] = "s32e", [XT_S32NB] = "s32nb",
    [XT_SALT] = "salt", [XT_SALTU] = "saltu",
    [XT_ADD_S] = "add.s", [XT_SUB_S] = "sub.s", [XT_MUL_S] = "mul.s", [XT_MADD_S] = "madd.s",
    [XT_MSUB_S] = "msub.s", [XT_MADDN_S] = "maddn.s", [XT_DIVN_S] = "divn.s", [XT_ROUND_S] = "round.s",
    [XT_TRUNC_S] = "trunc.s", [XT_FLOOR_S] = "floor.s", [XT_CEIL_S] = "ceil.s", [XT_FLOAT_S] = "float.s",
    [XT_UFLOAT_S] = "ufloat.s", [XT_UTRUNC_S] = "utrunc.s", [XT_MOV_S] = "mov.s", [XT_ABS_S] = "abs.s",
    [XT_CONST_S] = "const.s", [XT_RFR] = "rfr", [XT_WFR] = "wfr", [XT_NEG_S] = "neg.s",
    [XT_DIV0_S] = "div0.s", [XT_RECIP0_S] = "recip0.s", [XT_SQRT0_S] = "sqrt0.s",
    [XT_RSQRT0_S] = "rsqrt0.s", [XT_NEXP01_S] = "nexp01.s", [XT_MKSADJ_S] = "mksadj.s",
    [XT_MKDADJ_S] = "mkdadj.s", [XT_ADDEXP_S] = "addexp.s", [XT_ADDEXPM_S] = "addexpm.s",
    [XT_UN_S] = "un.s", [XT_OEQ_S] = "oeq.s", [XT_UEQ_S] = "ueq.s", [XT_OLT_S] = "olt.s",
    [XT_ULT_S] = "ult.s", [XT_OLE_S] = "ole.s", [XT_ULE_S] = "ule.s", [XT_MOVEQZ_S] = "moveqz.s",
    [XT_MOVNEZ_S] = "movnez.s", [XT_MOVLTZ_S] = "movltz.s", [XT_MOVGEZ_S] = "movgez.s",
    [XT_MOVF_S] = "movf.s", [XT_MOVT_S] = "movt.s",
    [XT_L32R] = "l32r", [XT_L8UI] = "l8ui", [XT_L16UI] = "l16ui", [XT_L32I] = "l32i", [XT_S8I] = "s8i",
    [XT_S16I] = "s16i", [XT_S32I] = "s32i", [XT_L16SI] = "l16si", [XT_MOVI] = "movi",
    [XT_L32AI] = "l32ai", [XT_ADDI] = "addi", [XT_ADDMI] = "addmi", [XT_S32C1I] = "s32c1i",
    [XT_S32RI] = "s32ri",
    [XT_DPFR] = "dpfr", [XT_DPFW] = "dpfw", [XT_DPFRO] = "dpfro", [XT_DPFWO] = "dpfwo",
    [XT_DHWB] = "dhwb", [XT_DHWBI] = "dhwbi", [XT_DHI] = "dhi", [XT_DII] = "dii", [XT_DPFL] = "dpfl",
    [XT_DHU] = "dhu", [XT_DIU] = "diu", [XT_DIWB] = "diwb", [XT_DIWBI] = "diwbi", [XT_IPF] = "ipf",
    [XT_IPFL] = "ipfl", [XT_IHU] = "ihu", [XT_IIU] = "iiu", [XT_IHI] = "ihi", [XT_III] = "iii",
    [XT_LSI] = "lsi", [XT_SSI] = "ssi", [XT_LSIU] = "lsip", [XT_SSIU] = "ssip",
    [XT_CALL0] = "call0", [XT_CALL4] = "call4", [XT_CALL8] = "call8", [XT_CALL12] = "call12",
    [XT_J] = "j", [XT_BEQZ] = "beqz", [XT_BNEZ] = "bnez", [XT_BLTZ] = "bltz", [XT_BGEZ] = "bgez",
    [XT_BEQI] = "beqi", [XT_BNEI] = "bnei", [XT_BLTI] = "blti", [XT_BGEI] = "bgei",
    [XT_ENTRY] = "entry", [XT_BF] = "bf", [XT_BT] = "bt", [XT_LOOP] = "loop", [XT_LOOPNEZ] = "loopnez",
    [XT_LOOPGTZ] = "loopgtz", [XT_BLTUI] = "bltui", [XT_BGEUI] = "bgeui",
    [XT_BNONE] = "bnone", [XT_BEQ] = "beq", [XT_BLT] = "blt", [XT_BLTU] = "bltu", [XT_BALL] = "ball",
    [XT_BBC] = "bbc", [XT_BBCI] = "bbci", [XT_BANY] = "bany", [XT_BNE] = "bne", [XT_BGE] = "bge",
    [XT_BGEU] = "bgeu", [XT_BNALL] = "bnall", [XT_BBS] = "bbs", [XT_BBSI] = "bbsi",
    [XT_L32I_N] = "l32i.n", [XT_S32I_N] = "s32i.n", [XT_ADD_N] = "add.n", [XT_ADDI_N] = "addi.n",
    [XT_MOVI_N] = "movi.n", [XT_BEQZ_N] = "beqz.n", [XT_BNEZ_N] = "bnez.n", [XT_MOV_N] = "mov.n",
    [XT_RET_N] = "ret.n", [XT_RETW_N] = "retw.n", [XT_BREAK_N] = "break.n", [XT_NOP_N] = "nop.n",
    [XT_ILL_N] = "ill.n",
};

void xt_disasm(const XtInsn *d, uint32_t pc, char *out, size_t n)
{
    const unsigned r = d->r, s = d->s, t = d->t;
    const char *nm = names[d->op] ? names[d->op] : "?";
    const uint32_t target = xt_target(d, pc);
    const uint32_t bitnum = t | ((r & 1u) << 4);
    char nb[16];

    switch (d->op) {
    case XT_UNKNOWN:
        snprintf(out, n, "?");
        return;
    case XT_LDINC: case XT_LDDEC:
        snprintf(out, n, "%s\tm%u, a%u", d->op == XT_LDINC ? "ldinc" : "lddec", r & 3, s);
        return;
    case XT_MAC16: {
        static const char *const kinds[4] = { "umul", "mul", "mula", "muls" };
        static const char *const halves[4] = { "ll", "hl", "lh", "hh" };
        const char *k = kinds[XT_MAC_KIND(d->imm)], *h = halves[XT_MAC_HALF(d->imm)];
        const unsigned mx = (r >> 2) & 1, my = 2 + ((t >> 2) & 1), mw = r & 3;
        switch (XT_MAC_FORM(d->imm)) {
        case XT_MAC_AA: snprintf(out, n, "%s.aa.%s\ta%u, a%u", k, h, s, t); break;
        case XT_MAC_AD: snprintf(out, n, "%s.ad.%s\ta%u, m%u", k, h, s, my); break;
        case XT_MAC_DA: snprintf(out, n, "%s.da.%s\tm%u, a%u", k, h, mx, t); break;
        case XT_MAC_DD: snprintf(out, n, "%s.dd.%s\tm%u, m%u", k, h, mx, my); break;
        case XT_MAC_DD_LDINC: case XT_MAC_DD_LDDEC:
            snprintf(out, n, "%s.dd.%s.%s\tm%u, a%u, m%u, m%u", k, h,
                     XT_MAC_FORM(d->imm) == XT_MAC_DD_LDINC ? "ldinc" : "lddec", mw, s, mx, my);
            break;
        default:
            snprintf(out, n, "%s.da.%s.%s\tm%u, a%u, m%u, a%u", k, h,
                     XT_MAC_FORM(d->imm) == XT_MAC_DA_LDINC ? "ldinc" : "lddec", mw, s, mx, t);
            break;
        }
        return;
    }
    /* no operands */
    case XT_ILL: case XT_RET: case XT_RETW: case XT_ISYNC: case XT_RSYNC: case XT_ESYNC: case XT_DSYNC:
    case XT_EXCW: case XT_MEMW: case XT_EXTW: case XT_NOP: case XT_RFE: case XT_RFUE: case XT_RFDE:
    case XT_RFWO: case XT_RFWU: case XT_RFDO: case XT_RFDD: case XT_SYSCALL: case XT_SIMCALL:
    case XT_RET_N: case XT_RETW_N: case XT_NOP_N: case XT_ILL_N:
        snprintf(out, n, "%s", nm);
        return;
    /* ar, as, at */
    case XT_AND: case XT_OR: case XT_XOR: case XT_ADD: case XT_ADDX2: case XT_ADDX4: case XT_ADDX8:
    case XT_SUB: case XT_SUBX2: case XT_SUBX4: case XT_SUBX8: case XT_SRC: case XT_MUL16U: case XT_MUL16S:
    case XT_MULL: case XT_MULUH: case XT_MULSH: case XT_QUOU: case XT_QUOS: case XT_REMU: case XT_REMS:
    case XT_MIN: case XT_MAX: case XT_MINU: case XT_MAXU: case XT_MOVEQZ: case XT_MOVNEZ:
    case XT_MOVLTZ: case XT_MOVGEZ: case XT_ADD_N:
        snprintf(out, n, "%s\ta%u, a%u, a%u", nm, r, s, t);
        return;
    case XT_NEG: case XT_ABS: case XT_SRL: case XT_SRA:
        snprintf(out, n, "%s\ta%u, a%u", nm, r, t);
        return;
    case XT_SLL:
        snprintf(out, n, "%s\ta%u, a%u", nm, r, s);
        return;
    case XT_JX: case XT_CALLX0: case XT_CALLX4: case XT_CALLX8: case XT_CALLX12:
    case XT_SSR: case XT_SSL: case XT_SSA8L: case XT_SSA8B:
        snprintf(out, n, "%s\ta%u", nm, s);
        return;
    case XT_MOVSP: case XT_NSA: case XT_NSAU: case XT_RER: case XT_WER: case XT_MOV_N:
    case XT_RITLB0: case XT_PITLB: case XT_WITLB: case XT_RITLB1: case XT_RDTLB0: case XT_PDTLB:
    case XT_WDTLB: case XT_RDTLB1:
        snprintf(out, n, "%s\ta%u, a%u", nm, t, s);
        return;
    case XT_IITLB: case XT_IDTLB:
        snprintf(out, n, "%s\ta%u", nm, s);
        return;
    case XT_RFI: case XT_WAITI: case XT_BREAK_N:
        snprintf(out, n, "%s\t%u", nm, s);
        return;
    case XT_BREAK:
        snprintf(out, n, "%s\t%u, %u", nm, s, t);
        return;
    case XT_RSIL:
        snprintf(out, n, "%s\ta%u, %u", nm, t, s);
        return;
    case XT_ANY4: case XT_ALL4:
        snprintf(out, n, "%s\tb%u, b%u:b%u:b%u:b%u", nm, t, s, s + 1, s + 2, s + 3);
        return;
    case XT_ANY8: case XT_ALL8:
        snprintf(out, n, "%s\tb%u, b%u:b%u:b%u:b%u:b%u:b%u:b%u:b%u", nm, t, s, s + 1, s + 2, s + 3,
                 s + 4, s + 5, s + 6, s + 7);
        return;
    case XT_SSAI: case XT_ROTW:
        snprintf(out, n, "%s\t%d", nm, d->imm);
        return;
    case XT_SLLI:
        snprintf(out, n, "%s\ta%u, a%u, %d", nm, r, s, d->imm);
        return;
    case XT_SRAI: case XT_SRLI:
        snprintf(out, n, "%s\ta%u, a%u, %d", nm, r, t, d->imm);
        return;
    case XT_EXTUI:
        snprintf(out, n, "%s\ta%u, a%u, %d, %d", nm, r, t, d->imm & 0xff, d->imm >> 8);
        return;
    case XT_RSR: case XT_WSR: case XT_XSR: {
        const char *sr = xt_sr_name((unsigned)d->imm, d->op != XT_RSR);
        if (sr) snprintf(out, n, "%s.%s\ta%u", nm, sr, t);
        else snprintf(out, n, "%s\ta%u, %d", nm, t, d->imm);
        return;
    }
    case XT_RUR: case XT_WUR: {
        const char *ur = ur_name((unsigned)d->imm);
        const unsigned reg = d->op == XT_RUR ? r : t;
        if (ur) snprintf(out, n, "%s.%s\ta%u", nm, ur, reg);
        else snprintf(out, n, "%s\ta%u, %d", nm, reg, d->imm);
        return;
    }
    case XT_SEXT: case XT_CLAMPS:
        snprintf(out, n, "%s\ta%u, a%u, %d", nm, r, s, d->imm);
        return;
    case XT_MOVF: case XT_MOVT:
        snprintf(out, n, "%s\ta%u, a%u, b%u", nm, r, s, t);
        return;
    case XT_ANDB: case XT_ANDBC: case XT_ORB: case XT_ORBC: case XT_XORB:
        snprintf(out, n, "%s\tb%u, b%u, b%u", nm, r, s, t);
        return;
    case XT_LSX: case XT_LSXP: case XT_SSX: case XT_SSXP:
        snprintf(out, n, "%s\tf%u, a%u, a%u", nm, r, s, t);
        return;
    case XT_L32E: case XT_S32E: case XT_S32NB:
    case XT_L8UI: case XT_L16UI: case XT_L32I: case XT_S8I: case XT_S16I: case XT_S32I: case XT_L16SI:
    case XT_L32AI: case XT_ADDI: case XT_S32C1I: case XT_S32RI: case XT_L32I_N: case XT_S32I_N:
        snprintf(out, n, "%s\ta%u, a%u, %s", nm, t, s, num(d->imm, nb));
        return;
    case XT_SALT: case XT_SALTU:
        snprintf(out, n, "%s\ta%u, a%u, a%u", nm, r, s, t);
        return;
    case XT_ADDMI:
        snprintf(out, n, "%s\ta%u, a%u, 0x%x", nm, t, s, (uint32_t)d->imm);
        return;
    case XT_ADDI_N:
        snprintf(out, n, "%s\ta%u, a%u, %d", nm, r, s, d->imm);
        return;
    case XT_MOVI:
        snprintf(out, n, "%s\ta%u, %s", nm, t, num(d->imm, nb));
        return;
    case XT_MOVI_N:
        snprintf(out, n, "%s\ta%u, %d", nm, s, d->imm);
        return;
    case XT_DPFR: case XT_DPFW: case XT_DPFRO: case XT_DPFWO: case XT_DHWB: case XT_DHWBI: case XT_DHI:
    case XT_DII: case XT_DPFL: case XT_DHU: case XT_DIU: case XT_DIWB: case XT_DIWBI: case XT_IPF:
    case XT_IPFL: case XT_IHU: case XT_IIU: case XT_IHI: case XT_III:
        snprintf(out, n, "%s\ta%u, %d", nm, s, d->imm);
        return;
    case XT_LSI: case XT_SSI: case XT_LSIU: case XT_SSIU:
        snprintf(out, n, "%s\tf%u, a%u, %s", nm, t, s, num(d->imm, nb));
        return;
    case XT_ADD_S: case XT_SUB_S: case XT_MUL_S: case XT_MADD_S: case XT_MSUB_S: case XT_MADDN_S:
    case XT_DIVN_S:
        snprintf(out, n, "%s\tf%u, f%u, f%u", nm, r, s, t);
        return;
    case XT_ROUND_S: case XT_TRUNC_S: case XT_FLOOR_S: case XT_CEIL_S: case XT_UTRUNC_S:
        snprintf(out, n, "%s\ta%u, f%u, %u", nm, r, s, t);
        return;
    case XT_FLOAT_S: case XT_UFLOAT_S:
        snprintf(out, n, "%s\tf%u, a%u, %u", nm, r, s, t);
        return;
    case XT_MOV_S: case XT_ABS_S: case XT_NEG_S: case XT_DIV0_S: case XT_RECIP0_S: case XT_SQRT0_S:
    case XT_RSQRT0_S: case XT_NEXP01_S: case XT_MKSADJ_S: case XT_MKDADJ_S: case XT_ADDEXP_S:
    case XT_ADDEXPM_S:
        snprintf(out, n, "%s\tf%u, f%u", nm, r, s);
        return;
    case XT_CONST_S:
        snprintf(out, n, "%s\tf%u, %u", nm, r, s);
        return;
    case XT_RFR:
        snprintf(out, n, "%s\ta%u, f%u", nm, r, s);
        return;
    case XT_WFR:
        snprintf(out, n, "%s\tf%u, a%u", nm, r, s);
        return;
    case XT_UN_S: case XT_OEQ_S: case XT_UEQ_S: case XT_OLT_S: case XT_ULT_S: case XT_OLE_S: case XT_ULE_S:
        snprintf(out, n, "%s\tb%u, f%u, f%u", nm, r, s, t);
        return;
    case XT_MOVEQZ_S: case XT_MOVNEZ_S: case XT_MOVLTZ_S: case XT_MOVGEZ_S:
        snprintf(out, n, "%s\tf%u, f%u, a%u", nm, r, s, t);
        return;
    case XT_MOVF_S: case XT_MOVT_S:
        snprintf(out, n, "%s\tf%u, f%u, b%u", nm, r, s, t);
        return;
    case XT_L32R:
        snprintf(out, n, "%s\ta%u, %x", nm, t, target);
        return;
    case XT_CALL0: case XT_CALL4: case XT_CALL8: case XT_CALL12: case XT_J:
        snprintf(out, n, "%s\t%x", nm, target);
        return;
    case XT_BEQZ: case XT_BNEZ: case XT_BLTZ: case XT_BGEZ: case XT_BEQZ_N: case XT_BNEZ_N:
    case XT_LOOP: case XT_LOOPNEZ: case XT_LOOPGTZ:
        snprintf(out, n, "%s\ta%u, %x", nm, s, target);
        return;
    case XT_BEQI: case XT_BNEI: case XT_BLTI: case XT_BGEI:
        snprintf(out, n, "%s\ta%u, %s, %x", nm, s, num(b4const[r], nb), target);
        return;
    case XT_BLTUI: case XT_BGEUI:
        snprintf(out, n, "%s\ta%u, %s, %x", nm, s, num(b4constu[r], nb), target);
        return;
    case XT_BF: case XT_BT:
        snprintf(out, n, "%s\tb%u, %x", nm, s, target);
        return;
    case XT_BBCI: case XT_BBSI:
        snprintf(out, n, "%s\ta%u, %u, %x", nm, s, bitnum, target);
        return;
    case XT_BNONE: case XT_BEQ: case XT_BLT: case XT_BLTU: case XT_BALL: case XT_BBC: case XT_BANY:
    case XT_BNE: case XT_BGE: case XT_BGEU: case XT_BNALL: case XT_BBS:
        snprintf(out, n, "%s\ta%u, a%u, %x", nm, s, t, target);
        return;
    case XT_ENTRY:
        snprintf(out, n, "%s\ta%u, %s", nm, s, num(d->imm, nb));
        return;
    }
    snprintf(out, n, "%s", nm);
}

/* b4const for the executing side */
int32_t xt_b4const(unsigned r) { return b4const[r & 15]; }
int32_t xt_b4constu(unsigned r) { return b4constu[r & 15]; }
