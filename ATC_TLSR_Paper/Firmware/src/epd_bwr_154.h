#pragma once

// Source-axis (vertical) offset of the 152x152 glass on this controller: RAM
// bit b drives glass row b-SRC_OFFSET, so content must be rendered shifted DOWN
// by SRC_OFFSET pixels and the RAM X window extended accordingly.
// Tune by measuring the blank strip below the border rectangle:
// 8 px = 1.45mm, 12 px = 2.18mm, 16 px = 2.91mm (1 px = 27.6mm/152).
// CONFIRMED on real hardware (v4 flash): offset is 8 px — with it the border
// rectangle landed edge-to-edge on all four sides.
#define BWR_154_SRC_OFFSET 8
#define BWR_154_SRC_BYTES ((152 + BWR_154_SRC_OFFSET + 7) / 8) // RAM bytes per line
#define BWR_154_CANVAS_H (BWR_154_SRC_BYTES * 8)              // render canvas height

uint8_t EPD_BWR_154_detect(void);
int8_t EPD_BWR_154_read_temp(void);
int8_t EPD_BWR_154_Display(unsigned char *image, unsigned char *red_image, int size, uint8_t full_or_partial);
void EPD_BWR_154_set_sleep(void);