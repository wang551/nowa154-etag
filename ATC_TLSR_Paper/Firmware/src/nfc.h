#pragma once
#include <stdint.h>
#include "main.h"

void init_nfc(void);
void nfc_logger_poll(void);        // main_loop 调用：RF 寄存器窗/IRQ 变化抓取（nfc.c）
void nfc_note_retention_wake(void); // user_init_deepRetn 调用：唤醒计数（楔死诊断）
uint8_t nfc_session_active(void);   // RF 会话进行中（app.c 据此禁 suspend，v19）
// BLE 在线 EEPROM 控制台（v20，ota.c case 8/9/A 分发）
uint8_t nfc_ee_console_write(uint16_t addr, const uint8_t *data, uint8_t len, uint8_t *rb);
uint8_t nfc_ee_console_read(uint16_t addr, uint8_t *dst, uint8_t len);
uint8_t nfc_ee_console_reset(uint8_t *rb); // rb[0..5]=FFE0/1/2/Status/TxEn/MainIrqM
uint8_t nfc_ee_console_diag(uint8_t *out); // out[0..8] 双路诊断（v21）
uint8_t nfc_reg_console_write(uint16_t addr, uint8_t v, uint8_t *rb);
// v33 RE 控制台（实验梯）：0x390 实际配置字写入 + 掩码同会话验证
uint8_t nfc_cfg_console_write(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t mode, uint8_t *rb);
uint8_t nfc_mask_console_test(uint8_t *rb);
uint8_t nfc_unstick_console(uint8_t *rb); // v33d：手动解卡（9 时钟 + 断电 20ms + 验证）
uint8_t nfc_ee_snapshot(uint16_t addr, uint16_t len); // v34：EEPROM 快照→flash 0x45000（cmd4 取回）
uint8_t nfc_ee_direct_write(uint16_t addr, const uint8_t *data, uint8_t len, uint8_t *rb); // v34：直写（非 RMW）
uint8_t nfc_ee_magic_write(uint16_t addr, const uint8_t *data, uint8_t len, uint8_t *rb); // v34：{CE,55} magic + 直写
uint8_t nfc_fifo_console_test(uint8_t mode, uint8_t *rb); // v39：TX FIFO 落点自环（rb 11B，见 nfc.c）
uint8_t nfc_txdis_console_set(uint8_t en, uint8_t *rb);            // v40：TX 禁用开关（rb[0]=状态）
uint8_t nfc_mute_console_set(uint8_t en, uint8_t *rb);             // v41：静默模式（CSN 常低+零 I²C，rb[0]=状态）
uint8_t nfc_vendorseq_console_run(const uint8_t *script, uint8_t bytes, uint8_t *rb); // v40：厂商序列回放（8B 定长记录脚本，rb 18B）

// 冷启动 NFC 探测结果（deep retention 唤醒不重跑，RAM 保留上次的值）
extern uint8_t nfc_probe_done; // 1 = 本次冷启动已跑过探测
extern uint8_t nfc_probe_ack;  // 1 = 0x57 在 CSN 低电平扫描中 ACK（屏显用）
