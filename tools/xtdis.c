/*
 * Disassembles part of a raw binary with the emulator's own decoder, for
 * looking at the ROM or code copied out of a flash image:
 *
 *   xtdis file.bin load_address start [end]
 *
 * e.g. xtdis esp8266_rom.bin 40000000 40000080 40000200. Addresses are hex.
 * A symbol file in the form of eagle.rom.addr.v6.ld (PROVIDE ( name = 0x... );)
 * can be passed with --syms to label calls and function starts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xt_decode.h"

typedef struct { uint32_t addr; char name[64]; } Sym;
static Sym *syms;
static int nsyms;

static void load_syms(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char name[64];
        unsigned addr;
        if (sscanf(line, " PROVIDE ( %63s = 0x%x", name, &addr) == 2) {
            syms = realloc(syms, (size_t)(nsyms + 1) * sizeof(Sym));
            syms[nsyms].addr = addr;
            snprintf(syms[nsyms].name, sizeof syms[nsyms].name, "%s", name);
            nsyms++;
        }
    }
    fclose(f);
}

static const char *sym_at(uint32_t a)
{
    for (int i = 0; i < nsyms; i++)
        if (syms[i].addr == a) return syms[i].name;
    return NULL;
}

int main(int argc, char **argv)
{
    int argi = 1;
    if (argc > 2 && !strcmp(argv[1], "--syms")) { load_syms(argv[2]); argi = 3; }
    if (argc - argi < 3) {
        fprintf(stderr, "usage: xtdis [--syms file.ld] file.bin load_address start [end]\n");
        return 2;
    }
    FILE *f = fopen(argv[argi], "rb");
    if (!f) { perror(argv[argi]); return 1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = calloc(1, (size_t)n + 4);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);
    const uint32_t base = (uint32_t)strtoul(argv[argi + 1], NULL, 16);
    uint32_t pc = (uint32_t)strtoul(argv[argi + 2], NULL, 16);
    const uint32_t end = argc - argi > 3 ? (uint32_t)strtoul(argv[argi + 3], NULL, 16) : pc + 0x100;
    while (pc < end && pc - base < (uint32_t)n) {
        const uint8_t *p = buf + (pc - base);
        const uint32_t w = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
        XtInsn d;
        xt_decode(w, &d);
        char text[128];
        xt_disasm(&d, pc, text, sizeof text);
        const char *s = sym_at(pc);
        if (s) printf("\n%08x <%s>:\n", pc, s);
        const uint32_t tgt = xt_target(&d, pc);
        const char *ts = tgt ? sym_at(tgt) : NULL;
        if (d.op == XT_L32R && pc - base < (uint32_t)n) {
            const uint32_t la = tgt;
            uint32_t lit = 0;
            if (la - base + 4 <= (uint32_t)n) memcpy(&lit, buf + (la - base), 4);
            const char *ls = sym_at(lit);
            printf("%08x:  %-8x %s   ; =%08x%s%s\n", pc, d.raw, text, lit, ls ? " " : "", ls ? ls : "");
        } else {
            printf("%08x:  %-8x %s%s%s%s\n", pc, d.raw, text, ts ? "   <" : "", ts ? ts : "", ts ? ">" : "");
        }
        pc += d.len ? d.len : 1;
    }
    return 0;
}
