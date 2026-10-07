/*
 * The ESPboy's display: a Sitronix ST7735S controller and its 1.44" 128x128
 * glass, modelled on the ST7735S datasheet V1.1 (st7735.c).
 */
#ifndef ST7735_H
#define ST7735_H

#include <stdbool.h>
#include <stdint.h>

#define ST7735_COLS  132
#define ST7735_ROWS  162            /* frame memory rows */
#define ST7735_LINES 132            /* rows the panel shows and MY mirrors over (GM=01) */

typedef struct {
    uint32_t gram[ST7735_ROWS][ST7735_COLS];    /* 18 bit as the frame memory holds it: R 17:12, G 11:6, B 5:0 */
    uint8_t  cmd;
    int      nparam;
    uint8_t  param[16];
    uint16_t xs, xe, ys, ye;
    uint16_t cx, cy;
    bool     writing;
    uint8_t  pix[3];
    int      npix;
    uint8_t  madctl, colmod;
    bool     sleeping, display_on, inverted, idle;
    bool     partial, scrolling;
    uint16_t psl, pel;          /* PTLAR: the rows partial mode shows */
    uint16_t tfa, vsa, bfa;     /* SCRLAR: top fixed, scrolling and bottom fixed lines */
    uint16_t ssa;               /* VSCSAD: the line shown first after the top fixed area */
    bool     rst_level;         /* RESX; the ESPboy ties it to the board's reset, so it is high */
    uint32_t frames_written;    /* times the address counter wrapped a whole window */
    uint32_t frame_starts;      /* times a RAMWR window went back up the screen: a new frame */
    uint16_t last_ys;
    uint64_t rd_bits;           /* what a read command shifts out, MSB first */
    int      rd_n;              /* bits of it left */
} St7735;

void st7735_power_on(St7735 *lcd);
void st7735_reset(St7735 *lcd);
void st7735_byte(St7735 *lcd, bool dc, uint8_t byte);
/* the next bit the panel drives on SDA after a read command (0 once done) */
int st7735_read_bit(St7735 *lcd);
/* the glass as it looks now, 128x128 XRGB8888, top left first */
void st7735_render(const St7735 *lcd, uint32_t out[128 * 128]);

#endif
