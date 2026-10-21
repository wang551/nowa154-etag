#include "epd_ble_service.h"

#include <stdint.h>
#include "tl_common.h"
#include "stack/ble/ble.h"

#include "epd.h"
#include "ble.h"

#define ASSERT_MIN_LEN(val, min_len) \
	if (val < min_len)               \
	{                                \
		return 0;                    \
	}

extern unsigned char epd_buffer[epd_buffer_size];
// Red plane storage aliases epd_temp (the clock's OBD canvas): the two paths
// never overlap — the clock only redraws while the image is not held, and every
// upload starts with 0x00 clearing both planes. A dedicated 3 KB buffer would
// eat most of the ~4 KB SRAM left below _end_bss_ for the stack.
extern unsigned char epd_temp[epd_buffer_size];
unsigned int byte_pos = 0;
uint8_t epd_plane = 0; // 0x03 write target: 0 = b/w plane (epd_buffer), 1 = red plane (epd_temp)
extern void epd_resume_clock(void);

int epd_ble_handle_write(void *p)
{
	rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;
	uint8_t *payload = &req->value;
	unsigned int payload_len = req->l2capLen - 3;

	ASSERT_MIN_LEN(payload_len, 1);

	switch (payload[0])
	{
	// Clear both EPD planes (b/w fill byte + red blanked) and reset write state.
	case 0x00:
		ASSERT_MIN_LEN(payload_len, 2);
		memset(epd_buffer, payload[1], sizeof(epd_buffer));
		memset(epd_temp, 0, sizeof(epd_temp));
		epd_plane = 0;
		byte_pos = 0;
		ble_set_connection_speed(40);
		return 0;
	// Push both planes to display.
	case 0x01:
		ble_set_connection_speed(200);
		EPD_Display(epd_buffer, epd_temp, epd_buffer_size, 1);
		return 0;
	// Set byte_pos.
	case 0x02:
		ASSERT_MIN_LEN(payload_len, 3);
		byte_pos = payload[1] << 8 | payload[2];
		return 0;
	// Write data to the image buffer selected by epd_plane.
	case 0x03:
		if (byte_pos + payload_len - 1 >= sizeof(epd_buffer) + 1)
		{
			return 0;
		}
		memcpy((epd_plane ? epd_temp : epd_buffer) + byte_pos, payload + 1, payload_len - 1);
		byte_pos += payload_len - 1;
		return 0;
	case 0x04: // decode & display a TIFF image
		epd_display_tiff(epd_buffer, byte_pos);
		return 0;
	case 0x05: // hold custom image: 1 = keep BLE image (skip clock), 0 = resume clock. Volatile, reboot clears it.
		ASSERT_MIN_LEN(payload_len, 2);
		epd_hold_image = payload[1] ? 1 : 0;
		if (!epd_hold_image)
			epd_resume_clock();
		return 0;
	case 0x06: // select 0x03 write target plane (1 = red) and rewind byte_pos.
		// Firmware without this command ignores it and keeps writing the b/w
		// plane, so the red upload degrades to a no-op there.
		ASSERT_MIN_LEN(payload_len, 2);
		epd_plane = payload[1] ? 1 : 0;
		byte_pos = 0;
		return 0;
	default:
		return 0;
	}

	return 0;
}
