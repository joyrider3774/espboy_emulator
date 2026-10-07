/*
 * Decoding of the Xtensa instruction set. The decoder knows the full set
 * of the Xtensa options, more than the LX106 has: the core
 * ISA, windowed registers, code density (16-bit instructions), loops,
 * multiply/divide, MIN/MAX, NSA, SEXT/CLAMPS, booleans, the FPU, MAC16,
 * S32C1I and the exception/interrupt instructions. The ESP8266's LX106 has
 * only part of it (core-isa.h: no windows, loops, divide, MIN/MAX, SEXT,
 * booleans, FPU or MAC16); xt_cpu.c executes what the LX106 has and raises
 * an illegal instruction exception for the rest.
 *
 * xt_disasm() prints an instruction the way GNU objdump does, which is what
 * tests/decode_test.c checks the decoder against, instruction by instruction.
 */
#ifndef XT_DECODE_H
#define XT_DECODE_H

#include <stdint.h>
#include <stddef.h>

enum {
    XT_UNKNOWN = 0,
    XT_ILL,
    /* RST0 */
    XT_AND, XT_OR, XT_XOR, XT_ADD, XT_ADDX2, XT_ADDX4, XT_ADDX8, XT_SUB, XT_SUBX2, XT_SUBX4, XT_SUBX8,
    XT_NEG, XT_ABS,
    XT_RET, XT_RETW, XT_JX, XT_CALLX0, XT_CALLX4, XT_CALLX8, XT_CALLX12,
    XT_MOVSP, XT_ISYNC, XT_RSYNC, XT_ESYNC, XT_DSYNC, XT_EXCW, XT_MEMW, XT_EXTW, XT_NOP,
    XT_RFE, XT_RFUE, XT_RFDE, XT_RFWO, XT_RFWU, XT_RFI, XT_RFDO, XT_RFDD,
    XT_BREAK, XT_SYSCALL, XT_SIMCALL, XT_RSIL, XT_WAITI,
    XT_ANY4, XT_ALL4, XT_ANY8, XT_ALL8,
    XT_SSR, XT_SSL, XT_SSA8L, XT_SSA8B, XT_SSAI, XT_RER, XT_WER, XT_ROTW, XT_NSA, XT_NSAU,
    XT_RITLB0, XT_IITLB, XT_PITLB, XT_WITLB, XT_RITLB1, XT_RDTLB0, XT_IDTLB, XT_PDTLB, XT_WDTLB, XT_RDTLB1,
    /* RST1 */
    XT_SLLI, XT_SRAI, XT_SRLI, XT_XSR, XT_SRC, XT_SRL, XT_SLL, XT_SRA, XT_MUL16U, XT_MUL16S,
    /* RST2 */
    XT_ANDB, XT_ANDBC, XT_ORB, XT_ORBC, XT_XORB,
    XT_SALT, XT_SALTU, XT_MULL, XT_MULUH, XT_MULSH, XT_QUOU, XT_QUOS, XT_REMU, XT_REMS,
    /* RST3 */
    XT_RSR, XT_WSR, XT_SEXT, XT_CLAMPS, XT_MIN, XT_MAX, XT_MINU, XT_MAXU,
    XT_MOVEQZ, XT_MOVNEZ, XT_MOVLTZ, XT_MOVGEZ, XT_MOVF, XT_MOVT, XT_RUR, XT_WUR,
    XT_EXTUI,
    /* LSCX, LSC4 */
    XT_LSX, XT_LSXP, XT_SSX, XT_SSXP, XT_L32E, XT_S32E, XT_S32NB,
    /* FP0 */
    XT_ADD_S, XT_SUB_S, XT_MUL_S, XT_MADD_S, XT_MSUB_S, XT_MADDN_S, XT_DIVN_S,
    XT_ROUND_S, XT_TRUNC_S, XT_FLOOR_S, XT_CEIL_S, XT_FLOAT_S, XT_UFLOAT_S, XT_UTRUNC_S,
    XT_MOV_S, XT_ABS_S, XT_CONST_S, XT_RFR, XT_WFR, XT_NEG_S, XT_DIV0_S, XT_RECIP0_S, XT_SQRT0_S,
    XT_RSQRT0_S, XT_NEXP01_S, XT_MKSADJ_S, XT_MKDADJ_S, XT_ADDEXP_S, XT_ADDEXPM_S,
    /* FP1 */
    XT_UN_S, XT_OEQ_S, XT_UEQ_S, XT_OLT_S, XT_ULT_S, XT_OLE_S, XT_ULE_S,
    XT_MOVEQZ_S, XT_MOVNEZ_S, XT_MOVLTZ_S, XT_MOVGEZ_S, XT_MOVF_S, XT_MOVT_S,
    /* L32R, LSAI, LSCI */
    XT_L32R,
    XT_L8UI, XT_L16UI, XT_L32I, XT_S8I, XT_S16I, XT_S32I, XT_L16SI, XT_MOVI, XT_L32AI,
    XT_ADDI, XT_ADDMI, XT_S32C1I, XT_S32RI,
    XT_DPFR, XT_DPFW, XT_DPFRO, XT_DPFWO, XT_DHWB, XT_DHWBI, XT_DHI, XT_DII, XT_DPFL, XT_DHU, XT_DIU,
    XT_DIWB, XT_DIWBI, XT_IPF, XT_IPFL, XT_IHU, XT_IIU, XT_IHI, XT_III,
    XT_LSI, XT_SSI, XT_LSIU, XT_SSIU,
    /* CALLN, SI, B */
    XT_CALL0, XT_CALL4, XT_CALL8, XT_CALL12,
    XT_J, XT_BEQZ, XT_BNEZ, XT_BLTZ, XT_BGEZ, XT_BEQI, XT_BNEI, XT_BLTI, XT_BGEI,
    XT_ENTRY, XT_BF, XT_BT, XT_LOOP, XT_LOOPNEZ, XT_LOOPGTZ, XT_BLTUI, XT_BGEUI,
    XT_BNONE, XT_BEQ, XT_BLT, XT_BLTU, XT_BALL, XT_BBC, XT_BBCI, XT_BANY, XT_BNE, XT_BGE, XT_BGEU,
    XT_BNALL, XT_BBS, XT_BBSI,
    /* density */
    XT_L32I_N, XT_S32I_N, XT_ADD_N, XT_ADDI_N, XT_MOVI_N, XT_BEQZ_N, XT_BNEZ_N, XT_MOV_N,
    XT_RET_N, XT_RETW_N, XT_BREAK_N, XT_NOP_N, XT_ILL_N,
    /* MAC16: one op for all the multiplies, imm = form | kind << 4 | half << 6 */
    XT_MAC16, XT_LDINC, XT_LDDEC,
    XT_OP_COUNT
};

/* XT_MAC16's imm: the form (op2), the kind and which 16-bit halves */
enum {
    XT_MAC_DD_LDINC = 0, XT_MAC_DD_LDDEC = 1, XT_MAC_DD = 2, XT_MAC_AD = 3,
    XT_MAC_DA_LDINC = 4, XT_MAC_DA_LDDEC = 5, XT_MAC_DA = 6, XT_MAC_AA = 7,
};
#define XT_MAC_FORM(imm)  ((imm) & 15)
#define XT_MAC_KIND(imm)  (((imm) >> 4) & 3)     /* 0 umul, 1 mul, 2 mula, 3 muls */
#define XT_MAC_HALF(imm)  (((imm) >> 6) & 3)     /* bit 0: first operand high, bit 1: second high */

typedef struct {
    uint16_t op;
    uint8_t  len;       /* 2 or 3 bytes */
    uint8_t  r, s, t;   /* the raw register fields */
    uint16_t regs;      /* address registers it names (decoder); xt_cpu.c narrows it to the ones it reads */
    int32_t  imm;       /* immediate, offset or special register number, per op */
    uint32_t raw;
} XtInsn;

/* Decodes the instruction whose bytes (little endian) are in 'word'; for
   branches and calls 'imm' is the offset from the address the ISA counts from */
void xt_decode(uint32_t word, XtInsn *d);

/* Branch / call / loop / L32R target of the instruction at pc, or 0 */
uint32_t xt_target(const XtInsn *d, uint32_t pc);

/* objdump's text for it: "mnemonic\toperands", no symbols */
void xt_disasm(const XtInsn *d, uint32_t pc, char *out, size_t outlen);

const char *xt_sr_name(unsigned sr, int write);

#endif
