#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "drivers/8258/flash.h"
#include "ota.h"
#include "main.h"
#include "nfc.h"

#define OTA_BANK_START 0x20000 // 131kb about
#define OTA_MAX_SIZE 0x20000   // 131kb about

RAM uint8_t out_buffer[20] = {0};

RAM uint8_t ramd_to_flash_temp_buffer[0x100];
RAM uint16_t ram_position = 0;

uint16_t crc_out = 0;

_attribute_ram_code_ int custom_otaWrite(void *p)
{
	rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;
	uint8_t *payload = &req->value;
	uint8_t data_len = req->l2capLen - 3;
	uint32_t address = 0;
	uint32_t read_offset = 0;
	if (data_len >= 5)
	{
		address = (payload[1] << 24) | (payload[2] << 16) | (payload[3] << 8) | payload[4];
	}

	switch (payload[0])
	{
	case 0:																						   // just a reboot to test
		analog_write(SYS_DEEP_ANA_REG, analog_read(SYS_DEEP_ANA_REG) & (~SYS_NEED_REINIT_EXT32K)); // clear
		irq_disable();
		REG_ADDR8(0x6f) = 0x20; // reboot
		while (1)
		{
		}
		break;
	case 1: // erasing a sector of the flash, better be careful here ^^ could erase the running firmware
		crc_out = 0;
		if (address >= OTA_BANK_START && address < (OTA_BANK_START + OTA_MAX_SIZE - 0x100))
		{
			flash_erase_sector(address);
		}
		memset(ramd_to_flash_temp_buffer, 0x00, sizeof(ramd_to_flash_temp_buffer));
		ram_position = 0;
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, payload, data_len);
		break;
	case 2: // writing one bank (256byte) to flash at given sector
		crc_out = 0;
		if (address >= OTA_BANK_START && address < (OTA_BANK_START + OTA_MAX_SIZE - 0x100))
		{
			flash_write_page(address, ram_position, ramd_to_flash_temp_buffer);
		}
		memset(ramd_to_flash_temp_buffer, 0x00, sizeof(ramd_to_flash_temp_buffer));
		ram_position = 0;
		// bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, payload, data_len);
		break;
	case 3: // write into the temporary buffer that will later be written to flash
		crc_out = 0;
		if (ram_position + (data_len - 1) > 0x100)
			return 0;
		memcpy(&ramd_to_flash_temp_buffer[ram_position], &payload[1], (data_len - 1));
		ram_position += (data_len - 1);
		// bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, &ram_position, sizeof(ram_position));
		break;
	case 4: // read real flash to verify
		crc_out = 0;
		flash_read_page(address, sizeof(out_buffer), out_buffer);
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, sizeof(out_buffer));
		break;
	case 5: // read real flash to verify
		crc_out = 0;
		memcpy(out_buffer, &ramd_to_flash_temp_buffer[address], sizeof(out_buffer));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, sizeof(out_buffer));
		break;
	case 6: // CRC Checking the uploaded part
		for (int i = 0; i < OTA_MAX_SIZE; i += 0x100)
		{
			flash_read_page(OTA_BANK_START + i, sizeof(ramd_to_flash_temp_buffer), ramd_to_flash_temp_buffer);
			for (int c = 0; c < 0x100; c++)
			{
				crc_out += ramd_to_flash_temp_buffer[c]; // yeah thats not real CRC, but its better than nothing for now
			}
		}
		out_buffer[0] = 0x07;
		out_buffer[1] = crc_out >> 8;
		out_buffer[2] = crc_out;
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 3);
		break;
	case 7:																									  // when upload is done flash the firmware with this cmd, better do some CRC or checking before as it could brick the device
		if ((address == 0xC001CEED) && (out_buffer[5] == (crc_out >> 8)) && (out_buffer[6] == (crc_out & 0xff))) // Only flash if "magic word" is send
			write_ota_firmware_to_flash();
		break;
	case 8:
	{ // NFC EEPROM 写（v20 在线控制台）：08 <addr_hi> <addr_lo> <data...>（I2C 用户区 0x010-0x3FE）
		// notify: [0x88, ret, rb...]（rb=写后读回，最多 12B）
		crc_out = 0;
		uint8_t rb[12] = {0};
		uint8_t ret = 0xFF;
		if (data_len >= 4)
		{
			uint16_t ee_addr = (payload[1] << 8) | payload[2];
			uint8_t ee_len = data_len - 3;
			if (ee_len > sizeof(rb))
				ee_len = sizeof(rb);
			ret = nfc_ee_console_write(ee_addr, &payload[3], ee_len, rb);
		}
		out_buffer[0] = 0x88;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + sizeof(rb));
		break;
	}
	case 9:
	{ // NFC EEPROM 读：09 <addr_hi> <addr_lo> <len(1-16)> → notify [0x89, ret, data...]
		// v33f 修正：长度字节是 payload[3]（原 payload[4] 越界读陈旧缓冲，长度时对时错）
		crc_out = 0;
		uint8_t rb[16] = {0};
		uint8_t ret = 0xFF;
		uint8_t ee_len = 0;
		if (data_len >= 4) // 09 addr_hi addr_lo len
		{
			uint16_t ee_addr = (payload[1] << 8) | payload[2];
			ee_len = payload[3] > 16 ? 16 : payload[3];
			ret = nfc_ee_console_read(ee_addr, rb, ee_len);
		}
		out_buffer[0] = 0x89;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, ee_len);
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + ee_len);
		break;
	}
	case 0xA:
	{ // NFC 芯片软复位（重读 EEPROM 配置）→ notify [0x8A, ret, FFE0, FFE1, FFE2, Status, TxEn, MainIrqM]
		crc_out = 0;
		uint8_t rb[6] = {0};
		uint8_t ret = nfc_ee_console_reset(rb);
		out_buffer[0] = 0x8A;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + sizeof(rb));
		break;
	}
	case 0xB:
	{ // NFC 双路诊断（会话中可用）→ notify [0x8B, ret, cnt映射, cnt命令, st映射, st命令, MainIrq, Rats, TxEn, AuxIrq, NfcCfg]
		crc_out = 0;
		uint8_t rb[9];
		uint8_t ret = nfc_ee_console_diag(rb);
		out_buffer[0] = 0x8B;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + sizeof(rb));
		break;
	}
	case 0xC:
	{ // NFC 寄存器窗写：0C <hi> <lo> <val> → notify [0x8C, ret, rb]（rb=写后 2ms 读回）
		// v33f 修正：帧共 4 字节（data_len>=4；原 >=5 使本命令自 v20 起从未执行过）
		crc_out = 0;
		uint8_t rb = 0xFF;
		uint8_t ret = 2;
		if (data_len >= 4)
			ret = nfc_reg_console_write((payload[1] << 8) | payload[2], payload[3], &rb);
		out_buffer[0] = 0x8C;
		out_buffer[1] = ret;
		out_buffer[2] = rb;
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 3);
		break;
	}
	case 0xD:
	{ // v33 配置字写：0D <c0> <c1> <c2> <mode> → notify [0x8D, ret, 390×4, FFE0, FFE1, FFE2, AuxIrq, MIM]
		crc_out = 0;
		uint8_t rb[9] = {0};
		uint8_t ret = 0xFF;
		if (data_len >= 5)
			ret = nfc_cfg_console_write(payload[1], payload[2], payload[3], payload[4], rb);
		out_buffer[0] = 0x8D;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + sizeof(rb));
		break;
	}
	case 0xE:
	{ // v33 掩码同会话测试：0E → notify [0x8E, ret, 写前, 即读, +2ms, +10ms, AuxIrq]
		crc_out = 0;
		uint8_t rb[5] = {0};
		uint8_t ret = nfc_mask_console_test(rb);
		out_buffer[0] = 0x8E;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + sizeof(rb));
		break;
	}
	case 0xF:
	{ // v33d 手动解卡：0F → notify [0x8F, ret, FFF1..FFF6]
		crc_out = 0;
		uint8_t rb[6] = {0};
		uint8_t ret = nfc_unstick_console(rb);
		out_buffer[0] = 0x8F;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + sizeof(rb));
		break;
	}
	case 0x10:
	{ // v34 EEPROM 快照：10 <hi> <lo> <lenHi> <lenLo> → notify [0x90, ret]（数据随后用 cmd4 读 0x45000）
		crc_out = 0;
		uint8_t ret = 1;
		if (data_len >= 5)
			ret = nfc_ee_snapshot((payload[1] << 8) | payload[2], (payload[3] << 8) | payload[4]);
		out_buffer[0] = 0x90;
		out_buffer[1] = ret;
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2);
		break;
	}
	case 0x11:
	{ // v34 EEPROM 直写：11 <hi> <lo> <bytes...> → notify [0x91, ret, AuxIrq, 读回...]
		crc_out = 0;
		uint8_t rb[12] = {0};
		uint8_t ret = 1;
		uint8_t n = data_len >= 4 ? data_len - 3 : 0;
		if (n > 11)
			n = 11;
		if (n)
			ret = nfc_ee_direct_write((payload[1] << 8) | payload[2], &payload[3], n, rb);
		out_buffer[0] = 0x91;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, 1 + n);
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + 1 + n);
		break;
	}
	case 0x12:
	{ // v34 magic 写：12 <hi> <lo> <bytes...> → notify [0x92, ret, AuxIrq, 读回...]
		crc_out = 0;
		uint8_t rb[12] = {0};
		uint8_t ret = 1;
		uint8_t n = data_len >= 4 ? data_len - 3 : 0;
		if (n > 11)
			n = 11;
		if (n)
			ret = nfc_ee_magic_write((payload[1] << 8) | payload[2], &payload[3], n, rb);
		out_buffer[0] = 0x92;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, 1 + n);
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 2 + 1 + n);
		break;
	}
	case 0x13:
	{ // v39 TX FIFO 自环：13 <mode 0=内存窗 1=0x80命令 2=0x00命令> → notify [0x93, ret, mode, rb 11B]
		crc_out = 0;
		uint8_t rb[11] = {0};
		uint8_t ret = 2;
		uint8_t mode = 0;
		if (data_len >= 2)
		{
			mode = payload[1] & 3;
			ret = nfc_fifo_console_test(mode, rb);
		}
		out_buffer[0] = 0x93;
		out_buffer[1] = ret;
		out_buffer[2] = mode;
		memcpy(&out_buffer[3], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 3 + sizeof(rb));
		break;
	}
	case 0x14:
	{ // v40 vendorseq：脚本先经 case 3 帧上传 RAM 缓冲（8B 定长记录，≤30 条），再 14 <len> 执行
		// notify: [0x94, ret, n_op, 健康, read16B]（厂商接触侧配方回放，记录格式见 nfc.c）
		crc_out = 0;
		uint8_t rb[18] = {0};
		uint8_t ret = 0xFF;
		if (data_len >= 2)
		{
			ret = nfc_vendorseq_console_run(ramd_to_flash_temp_buffer, payload[1], rb);
			ram_position = 0; // 脚本已消费，RAM 缓冲归零防错位复用
		}
		out_buffer[0] = 0x94;
		out_buffer[1] = ret;
		memcpy(&out_buffer[2], rb, sizeof(rb));
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, sizeof(out_buffer));
		break;
	}
	case 0x15:
	{ // v40 TX 禁用开关：15 <0|1> → notify [0x95, ret, state]（§11.5 对照实验）
		crc_out = 0;
		uint8_t st = 0xFF;
		uint8_t ret = 0xFF;
		if (data_len >= 2)
			ret = nfc_txdis_console_set(payload[1], &st);
		out_buffer[0] = 0x95;
		out_buffer[1] = ret;
		out_buffer[2] = st;
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 3);
		break;
	}
	case 0x16:
	{ // v41 静默模式：16 <0|1> → notify [0x96, ret, state]（CSN 常低+零 I²C 流量，
		// 判芯片在"接触侧上电且总线静默"时是否自应答 T4T；RAM 态重启即清）
		crc_out = 0;
		uint8_t st = 0xFF;
		uint8_t ret = 0xFF;
		if (data_len >= 2)
			ret = nfc_mute_console_set(payload[1], &st);
		out_buffer[0] = 0x96;
		out_buffer[1] = ret;
		out_buffer[2] = st;
		bls_att_pushNotifyData(OTA_CMD_OUT_DP_H, out_buffer, 3);
		break;
	}
	}

	return 0;
}

_attribute_ram_code_ int write_ota_firmware_to_flash(void)
{
	irq_disable();
	uint32_t address = 0;
	while (address < OTA_MAX_SIZE)
	{
		flash_erase_sector(address);
		address += 0x1000;
	}
	address = 0;
	while (address < OTA_MAX_SIZE)
	{
		flash_read_page(OTA_BANK_START + address, 0x100, ramd_to_flash_temp_buffer);
		flash_write_page(address, 0x100, ramd_to_flash_temp_buffer);
		address += 0x100;
	}
	analog_write(SYS_DEEP_ANA_REG, analog_read(SYS_DEEP_ANA_REG) & (~SYS_NEED_REINIT_EXT32K));
	REG_ADDR8(0x6f) = 0x20;
	while (1)
	{
	}
}