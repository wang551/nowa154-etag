#include <stdint.h>
#include "tl_common.h"
#include "main.h"
#include "epd.h"
#include "epd_spi.h"
#include "epd_bw_213.h"
#include "epd_bwr_213.h"
#include "epd_bwr_350.h"
#include "epd_bwy_350.h"
#include "epd_bw_213_ice.h"
#include "epd_bwr_154.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "battery.h"

#include "OneBitDisplay.h"
#include "TIFF_G4.h"
#include "nfc.h"
extern const uint8_t ucMirror[];
#include "Roboto_Black_80.h"
#include "font_60.h"
#include "font16.h"
#include "font30.h"

RAM uint8_t epd_model = 0; // 0 = Undetected, 1 = BW213, 2 = BWR213, 3 = BWR154, 4 = BW213ICE, 5 = BWR350
const char *epd_model_string[] = {"NC", "BW213", "BWR213", "BWR154", "213ICE", "BWR350", "BWY350"};
RAM uint8_t epd_update_state = 0;
RAM uint8_t epd_hold_image = 0;

const char *BLE_conn_string[] = {"", "B"};
RAM uint8_t epd_temperature_is_read = 0;
RAM int8_t epd_temperature = 0; // 0x1B register: two's complement, negative below 0°C

uint8_t epd_buffer[epd_buffer_size];
uint8_t epd_temp[epd_buffer_size]; // for OneBitDisplay to draw into
OBDISP obd;                        // virtual display structure
TIFFIMAGE tiff;

// With this we can force a display if it wasnt detected correctly
void set_EPD_model(uint8_t model_nr)
{
    epd_model = model_nr;
}

// Here we detect what E-Paper display is connected
_attribute_ram_code_ void EPD_detect_model(void)
{
    EPD_init();
    // system power
    EPD_POWER_ON();

    WaitMs(10);
    // Reset the EPD driver IC
    gpio_write(EPD_RESET, 0);
    WaitMs(10);
    gpio_write(EPD_RESET, 1);
    WaitMs(10);

    // Here we neeed to detect it
    if (EPD_BWR_213_detect())
    {
        epd_model = 2;
    }
    else if (EPD_BWR_154_detect())// Right now this will never trigger, the 154 is same to 213BWR right now.
    {
        epd_model = 3;
    }
    else if (EPD_BW_213_ice_detect())
    {
        epd_model = 4;
    }
    else
    {
        epd_model = 1;
    }
    // Nowa-154R-N: the 154 controller answers the 213 probe the same way (branch above
    // always wins), so detection can never pick the square panel; force the 1.54" driver.
    epd_model = 3;

    EPD_POWER_OFF();
}

_attribute_ram_code_ int8_t EPD_read_temp(void)
{
    if (epd_temperature_is_read)
        return epd_temperature;

    if (!epd_model)
        EPD_detect_model();

    EPD_init();
    // system power
    EPD_POWER_ON();
    WaitMs(5);
    // Reset the EPD driver IC
    gpio_write(EPD_RESET, 0);
    WaitMs(10);
    gpio_write(EPD_RESET, 1);
    WaitMs(10);

    if (epd_model == 1)
        epd_temperature = EPD_BW_213_read_temp();
    else if (epd_model == 2)
        epd_temperature = EPD_BWR_213_read_temp();
    else if (epd_model == 3)
        epd_temperature = EPD_BWR_154_read_temp();
    else if (epd_model == 4)
        epd_temperature = EPD_BW_213_ice_read_temp();
    else if (epd_model == 5)
        epd_temperature = EPD_BWR_350_read_temp();
    else if (epd_model == 6)
        epd_temperature = EPD_BWY_350_read_temp();

    EPD_POWER_OFF();

    epd_temperature_is_read = 1;

    return epd_temperature;
}

_attribute_ram_code_ void EPD_Display(unsigned char *image, unsigned char *red_image, int size, uint8_t full_or_partial)
{
    if (!epd_model)
        EPD_detect_model();

    EPD_init();
    // system power
    EPD_POWER_ON();
    WaitMs(5);
    // Reset the EPD driver IC
    gpio_write(EPD_RESET, 0);
    WaitMs(10);
    gpio_write(EPD_RESET, 1);
    WaitMs(10);

    if (epd_model == 1)
        epd_temperature = EPD_BW_213_Display(image, size, full_or_partial);
    else if (epd_model == 2)
        epd_temperature = EPD_BWR_213_Display(image, size, full_or_partial);
    else if (epd_model == 3)
        epd_temperature = EPD_BWR_154_Display(image, red_image, size, full_or_partial);
    else if (epd_model == 4)
        epd_temperature = EPD_BW_213_ice_Display(image, size, full_or_partial);
    else if (epd_model == 5)
        epd_temperature = EPD_BWR_350_Display(image, size, full_or_partial);
    else if (epd_model == 6)
        epd_temperature = EPD_BWY_350_Display(image, size, full_or_partial);

    epd_temperature_is_read = 1;
    epd_update_state = 1;
}

_attribute_ram_code_ void epd_set_sleep(void)
{
    if (!epd_model)
        EPD_detect_model();

    if (epd_model == 1)
        EPD_BW_213_set_sleep();
    else if (epd_model == 2)
        EPD_BWR_213_set_sleep();
    else if (epd_model == 3)
        EPD_BWR_154_set_sleep();
    else if (epd_model == 4)
        EPD_BW_213_ice_set_sleep();
    else if (epd_model == 5)
        EPD_BWR_350_set_sleep();
    else if (epd_model == 6)
        EPD_BWY_350_set_sleep();

    EPD_POWER_OFF();
    epd_update_state = 0;
}

_attribute_ram_code_ uint8_t epd_state_handler(void)
{
    switch (epd_update_state)
    {
    case 0:
        // Nothing todo
        break;
    case 1: // check if refresh is done and sleep epd if so
        if (epd_model == 1)
        {
            if (!EPD_IS_BUSY())
                epd_set_sleep();
        }
        else
        {
            if (EPD_IS_BUSY())
                epd_set_sleep();
        }
        break;
    }
    return epd_update_state;
}

_attribute_ram_code_ void FixBuffer(uint8_t *pSrc, uint8_t *pDst, uint16_t width, uint16_t height)
{
    int x, y;
    uint8_t *s, *d;
    for (y = 0; y < (height / 8); y++)
    { // byte rows
        d = &pDst[y];
        s = &pSrc[y * width];
        for (x = 0; x < width; x++)
        {
            d[x * (height / 8)] = ~ucMirror[s[width - 1 - x]]; // invert and flip
        }                                                      // for x
    }                                                          // for y
}

// TIFF source geometry for TIFFDraw(), set by epd_display_tiff() per epd_model
RAM uint16_t epd_tiff_width = 250;
RAM uint16_t epd_tiff_stride = 16; // bytes per source column = (height+7)/8
RAM uint8_t epd_tiff_yoff = 0;     // extra source-axis offset (model 3 glass)

_attribute_ram_code_ void TIFFDraw(TIFFDRAW *pDraw)
{
    uint8_t uc = 0, ucSrcMask, ucDstMask, *s, *d;
    int x, y;

    s = pDraw->pPixels;
    y = pDraw->y;                                                        // current line
    d = &epd_buffer[((epd_tiff_width - 1) * epd_tiff_stride) + ((y + epd_tiff_yoff) / 8)]; // rotated 90 deg clockwise
    ucDstMask = 0x80 >> ((y + epd_tiff_yoff) & 7);                       // destination mask
    ucSrcMask = 0;                                                 // src mask
    for (x = 0; x < pDraw->iWidth; x++)
    {
        // Slower to draw this way, but it allows us to use a single buffer
        // instead of drawing and then converting the pixels to be the EPD format
        if (ucSrcMask == 0)
        { // load next source byte
            ucSrcMask = 0x80;
            uc = *s++;
        }
        if (!(uc & ucSrcMask))
        { // black pixel
            d[-(x * epd_tiff_stride)] &= ~ucDstMask;
        }
        ucSrcMask >>= 1;
    }
}

_attribute_ram_code_ void epd_display_tiff(uint8_t *pData, int iSize)
{
    uint16_t width = 250, height = 122;
    if (!epd_model)
        EPD_detect_model();
    if (epd_model == 3)
    { // square 1.54" panel, Nowa-154R-N is 152x152 behind a source-axis offset
        width = 152;
        height = 152;
        epd_tiff_stride = BWR_154_SRC_BYTES;
        epd_tiff_yoff = BWR_154_SRC_OFFSET;
    }
    else
    {
        epd_tiff_stride = (height + 7) / 8;
        epd_tiff_yoff = 0;
    }
    epd_tiff_width = width;

    // test G4 decoder
    memset(epd_buffer, 0xff, epd_buffer_size); // clear to white
    TIFF_openRAW(&tiff, width, height, BITDIR_MSB_FIRST, pData, iSize, TIFFDraw);
    TIFF_setDrawParameters(&tiff, 65536, TIFF_PIXEL_1BPP, 0, 0, width, height, NULL);
    TIFF_decode(&tiff);
    TIFF_close(&tiff);
    EPD_Display(epd_buffer, NULL, epd_buffer_size, 1);
}

extern uint8_t mac_public[6];

// Horizontal ink extents of a string (bitmap pixels, not advances), relative to
// the pen origin. Used to optically center/right-align text whose glyphs have
// narrow ink but wide advance (DSEG14 '1' is ~4px ink in a 29px advance, which
// makes advance-based centering land ~8px off true center).
static void epd_ink_extent(const GFXfont *pFont, const char *szMsg, int *left, int *right)
{
    int pen = 0, l = 0x7fff, r = -0x7fff;
    for (; *szMsg; szMsg++)
    {
        uint8_t c = (uint8_t)*szMsg;
        if (c < pFont->first || c > pFont->last)
            continue;
        const GFXglyph *g = &pFont->glyph[c - pFont->first];
        if (g->width)
        {
            if (pen + g->xOffset < l)
                l = pen + g->xOffset;
            if (pen + g->xOffset + g->width > r)
                r = pen + g->xOffset + g->width;
        }
        pen += g->xAdvance;
    }
    if (l > r)
        l = r = 0; // no printable glyphs
    *left = l;
    *right = r;
}

_attribute_ram_code_ void epd_display(uint32_t time_is, uint16_t battery_mv, int16_t temperature, uint8_t full_or_partial)
{
    if (epd_update_state)
        return;

    if (!epd_model)
    {
        EPD_detect_model();
    }
    uint16_t resolution_w = 250;
    uint16_t resolution_h = 128; // 122 real pixel, but needed to have a full byte
    if (epd_model == 1)
    {
        resolution_w = 250;
        resolution_h = 128; // 122 real pixel, but needed to have a full byte
    }
    else if (epd_model == 2)
    {
        resolution_w = 250;
        resolution_h = 128; // 122 real pixel, but needed to have a full byte
    }
    else if (epd_model == 3)
    {
        resolution_w = 152;
        resolution_h = BWR_154_CANVAS_H; // content is drawn shifted down by BWR_154_SRC_OFFSET
    }
    else if (epd_model == 4)
    {
        resolution_w = 212;
        resolution_h = 104;
    }
    else if (epd_model == 5)
    {// Just as placeholder right now, needs a complete different driving because of RAM limits
        resolution_w = 250;
        resolution_h = 128; // 122 real pixel, but needed to have a full byte
    }
    else if (epd_model == 6)
    {// Just as placeholder right now, needs a complete different driving because of RAM limits
        resolution_w = 250;
        resolution_h = 128; // 122 real pixel, but needed to have a full byte
    }

    uint16_t yoff = (epd_model == 3) ? BWR_154_SRC_OFFSET : 0;
    obdCreateVirtualDisplay(&obd, resolution_w, resolution_h, epd_temp);
    obdFill(&obd, 0, 0); // fill with white

    char buff[100];
    if (epd_model == 3)
    {
        // Square-glass dashboard (152x152, vision-reviewed): header line with BLE
        // status square, two rule lines, optically centered DSEG14 clock as the
        // focal element, temp/battery footer on a shared baseline, model caption.
        int l, r;
        sprintf(buff, "ESL_%02X%02X%02X", mac_public[2], mac_public[1], mac_public[0]);
        obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 8, yoff + 20, buff, 1);
        if (ble_get_connected()) // status LED, top-right
            obdRectangle(&obd, 136, yoff + 10, 143, yoff + 17, 1, 1);
        obdDrawLine(&obd, 8, yoff + 30, 143, yoff + 30, 1, 0);
        sprintf(buff, "%02d:%02d", ((time_is / 60) / 60) % 24, (time_is / 60) % 60);
        epd_ink_extent(&DSEG14_Classic_Mini_Regular_40, buff, &l, &r);
        obdWriteStringCustom(&obd, (GFXfont *)&DSEG14_Classic_Mini_Regular_40,
                            76 - (l + r) / 2, yoff + 84, buff, 1);
        obdDrawLine(&obd, 8, yoff + 96, 143, yoff + 96, 1, 0);
        sprintf(buff, "%d'C", EPD_read_temp());
        obdWriteStringCustom(&obd, (GFXfont *)&Special_Elite_Regular_30, 8, yoff + 124, buff, 1);
        sprintf(buff, "%d.%02dV", battery_mv / 1000, (battery_mv % 1000) / 10);
        epd_ink_extent(&Dialog_plain_16, buff, &l, &r);
        obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 143 - r, yoff + 124, buff, 1);
        // 机型标注行附带 NFC 探测结果（仅冷启动探测，见 nfc.c；57 = 0x57 ACK）
        char nfc_tag[20];
        if (nfc_probe_done && nfc_probe_ack)
            sprintf(nfc_tag, "%s NFC57", epd_model_string[epd_model]);
        else if (nfc_probe_done)
            sprintf(nfc_tag, "%s NFC--", epd_model_string[epd_model]);
        else
            sprintf(nfc_tag, "%s", epd_model_string[epd_model]);
        epd_ink_extent(&Dialog_plain_16, nfc_tag, &l, &r);
        obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 76 - (l + r) / 2, yoff + 144, nfc_tag, 1);
    }
    else
    {
        sprintf(buff, "ESL_%02X%02X%02X %s", mac_public[2], mac_public[1], mac_public[0], epd_model_string[epd_model]);
        obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 1, yoff + 17, (char *)buff, 1);
        sprintf(buff, "%s", BLE_conn_string[ble_get_connected()]);
        obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 132, yoff + 20, (char *)buff, 1);
        sprintf(buff, "%02d:%02d", ((time_is / 60) / 60) % 24, (time_is / 60) % 60);
        obdWriteStringCustom(&obd, (GFXfont *)&DSEG14_Classic_Mini_Regular_40, 50, yoff + 65, (char *)buff, 1);
        sprintf(buff, "%d'C", EPD_read_temp());
        obdWriteStringCustom(&obd, (GFXfont *)&Special_Elite_Regular_30, 10, yoff + 95, (char *)buff, 1);
        sprintf(buff, "Battery %dmV", battery_mv);
        obdWriteStringCustom(&obd, (GFXfont *)&Dialog_plain_16, 10, yoff + 120, (char *)buff, 1);
    }
    FixBuffer(epd_temp, epd_buffer, resolution_w, resolution_h);
    EPD_Display(epd_buffer, NULL, resolution_w * resolution_h / 8, full_or_partial);
}

_attribute_ram_code_ void epd_display_char(uint8_t data)
{
    int i;
    for (i = 0; i < epd_buffer_size; i++)
    {
        epd_buffer[i] = data;
    }
    EPD_Display(epd_buffer, NULL, epd_buffer_size, 1);
}

_attribute_ram_code_ void epd_clear(void)
{
    memset(epd_buffer, 0x00, epd_buffer_size);
}
