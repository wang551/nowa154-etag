#include <stdint.h>
#include "tl_common.h"
#include "main.h"
#include "epd.h"
#include "epd_spi.h"
#include "epd_bwr_154.h"
#include "drivers.h"
#include "stack/ble/ble.h"

// SSD1675 mixed with SSD1680 EPD Controller

// Nowa-154R-N panel: 152x152 (200x200 mapping drove sources past the glass:
// b/w noise on part of the screen, rest untouched)
#define BWR_154_WIDTH 152
#define BWR_154_HEIGHT 152
#define BWR_154_RAM_SIZE (BWR_154_WIDTH * BWR_154_SRC_BYTES) // incl. source offset pad
// Gates are NOT 0-based on Hanshow glass: the 213 tags use Y 46..296, and this
// 152-line glass maps to Y 144..295 (deduced from three flash experiments:
// window 0..199 -> ~56 lines visible, window 0..151 -> ~8 lines, both at the
// left edge; only a window ending at ~295 covers the full width).
#define BWR_154_GATE_START 295 // 0x0127, Y counter start (data entry Y decrement)
#define BWR_154_GATE_END 144   // 0x0090

#define BWR_154_Len 50
uint8_t LUT_bwr_154_part[] = {

0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

BWR_154_Len, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 
0x00, 0x00, 0x00, 

};

#define EPD_BWR_154_test_pattern 0xA5
_attribute_ram_code_ uint8_t EPD_BWR_154_detect(void)
{
    // SW Reset
    EPD_WriteCmd(0x12);
    WaitMs(10);

    EPD_WriteCmd(0x32);
    int i;
    for (i = 0; i < 65; i++)// This controller has a <100 bytes LUT storage so we test if thats existing.
    {
        EPD_WriteData(EPD_BWR_154_test_pattern);
    }
    EPD_WriteCmd(0x33);
    for (i = 0; i < 65; i++)
    {
        if(EPD_SPI_read() != EPD_BWR_154_test_pattern)
            return 0;
    }
    return 1;
}

_attribute_ram_code_ int8_t EPD_BWR_154_read_temp(void)
{
    int8_t epd_temperature = 0 ;
    
    // SW Reset
    EPD_WriteCmd(0x12);

    EPD_CheckStatus_inverted(100);

    EPD_WriteCmd(0x01);
    EPD_WriteData(0x28);
    EPD_WriteData(0x01);
    EPD_WriteData(0x01);

    // Data entry mode setting
    EPD_WriteCmd(0x11);
    EPD_WriteData(0x01);

    // Set RAM X- Address Start/End
    EPD_WriteCmd(0x44);
    EPD_WriteData(0x00);
    EPD_WriteData(0x18);

    // Set RAM Y- Address Start/End
    EPD_WriteCmd(0x45);
    EPD_WriteData(0x28);
    EPD_WriteData(0x01);
    EPD_WriteData(0x61);
    EPD_WriteData(0x00);

    // Border waveform control
    EPD_WriteCmd(0x3C);
    EPD_WriteData(0x05);

    // Temperature sensor control
    EPD_WriteCmd(0x18);
    EPD_WriteData(0x80);

    // Display update control
    EPD_WriteCmd(0x22);
    EPD_WriteData(0xB1);
    
    // Master Activation
    EPD_WriteCmd(0x20);

    EPD_CheckStatus_inverted(100);

    // Temperature sensor read from register
    EPD_WriteCmd(0x1B);
    epd_temperature = (int8_t)EPD_SPI_read();
    EPD_SPI_read();

    WaitMs(5);
    
    // deep sleep
    EPD_WriteCmd(0x10);
    EPD_WriteData(0x01);

    return epd_temperature;
}

_attribute_ram_code_ int8_t EPD_BWR_154_Display(unsigned char *image, unsigned char *red_image, int size, uint8_t full_or_partial)
{
    int8_t epd_temperature = 0 ;
    if (size > BWR_154_RAM_SIZE) // BLE raw path passes the 5000-byte buffer size
        size = BWR_154_RAM_SIZE;

    // SW Reset
    EPD_WriteCmd(0x12);

    EPD_CheckStatus_inverted(100);

    // Driver output control: last gate index 295, scan up
    EPD_WriteCmd(0x01);
    EPD_WriteData(BWR_154_GATE_START & 0xFF);
    EPD_WriteData(BWR_154_GATE_START >> 8);
    EPD_WriteData(0x01);

    // Data entry mode setting
    EPD_WriteCmd(0x11);
    EPD_WriteData(0x01);

    // Set RAM X- Address Start/End: covers 152 sources + source-axis offset
    EPD_WriteCmd(0x44);
    EPD_WriteData(0x00);
    EPD_WriteData(BWR_154_SRC_BYTES - 1);

    // Set RAM Y- Address Start/End: 295 -> 144
    EPD_WriteCmd(0x45);
    EPD_WriteData(BWR_154_GATE_START & 0xFF);
    EPD_WriteData(BWR_154_GATE_START >> 8);
    EPD_WriteData(BWR_154_GATE_END & 0xFF);
    EPD_WriteData(BWR_154_GATE_END >> 8);

    // Border waveform control
    EPD_WriteCmd(0x3C);
    EPD_WriteData(0x05);

    // Temperature sensor control
    EPD_WriteCmd(0x18);
    EPD_WriteData(0x80);

    // Display update control
    EPD_WriteCmd(0x22);
    EPD_WriteData(0xB1);
    
    // Master Activation
    EPD_WriteCmd(0x20);

    EPD_CheckStatus_inverted(100);

    // Temperature sensor read from register
    EPD_WriteCmd(0x1B);
    epd_temperature = (int8_t)EPD_SPI_read();
    EPD_SPI_read();

    WaitMs(5);

    // Set RAM X address
    EPD_WriteCmd(0x4E);
    EPD_WriteData(0x00);

    // Set RAM Y address
    EPD_WriteCmd(0x4F);
    EPD_WriteData(BWR_154_GATE_START & 0xFF);
    EPD_WriteData(BWR_154_GATE_START >> 8);

    EPD_LoadImage(image, size, 0x24);

    // Set RAM X address
    EPD_WriteCmd(0x4E);
    EPD_WriteData(0x00);

    // Set RAM Y address
    EPD_WriteCmd(0x4F);
    EPD_WriteData(BWR_154_GATE_START & 0xFF);
    EPD_WriteData(BWR_154_GATE_START >> 8);

    // Red plane (0x26 RAM, 1 = red). NULL means "no red": blank it so the glass
    // never shows stale red from a previous image.
    int i;
    if (red_image)
        EPD_LoadImage(red_image, size, 0x26);
    else
    {
        EPD_WriteCmd(0x26);
        for (i = 0; i < size; i++)
        {
            EPD_WriteData(0x00);
        }
    }

    if (!full_or_partial)
    {
        EPD_WriteCmd(0x32);
        for (i = 0; i < sizeof(LUT_bwr_154_part); i++)
        {
            EPD_WriteData(LUT_bwr_154_part[i]);
        }
    }
    
    // Display update control
    EPD_WriteCmd(0x22);
    EPD_WriteData(0xC7);
    
    // Master Activation
    EPD_WriteCmd(0x20);

    return epd_temperature;
}

_attribute_ram_code_ void EPD_BWR_154_set_sleep(void)
{
    // deep sleep
    EPD_WriteCmd(0x10);
    EPD_WriteData(0x01);

}