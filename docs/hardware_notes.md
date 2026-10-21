# 硬件笔记：汉朔 Nowa-154R-N

> 本文是公开版硬件鉴定结论，全部来自实物测试与公开资料（FCC 备案、厂商公开手册）。
> 版权 PDF 不随本仓库分发，文末给出公开获取链接。

## 型号解码（FCC 说明书官方定义）

`Nowa-154R-N`：

- `154` = 1.54 英寸，分辨率 **152×152**（说明书脚注：随批次可能不同；active area 27.6×27.6mm，即 1px ≈ 0.182mm）
- `R` = 黑/白/红三色屏
- `N` = 带 NFC 芯片

整机 48.7×35.8×12mm，IP50；电池为 550mAh 可换标准汉朔纽扣电池（CR2450 级）。

## 主控：Telink TLSR8359（B85 系列）

- 32 位 MCU + 2.4G 射频（BLE 栈 / 私有协议均可），512KB flash + 64KB SRAM
- 片上**没有 NFC 外设**（179 页数据手册全文确认）
- SWS 调试口复用在 **PA7**（与蓝色 LED 共用，烧录时会闪蓝灯属正常）
- 原厂固件跑汉朔私有 2.4G 协议（连汉朔 ESL-Working 基站），社区方案 = 直接换固件
- Telink IDE 里芯片系列选 **B85**；本仓库自带 tc32 工具链，无需安装 IDE

## NFC 芯片：复旦微 FM11NC08I（板上丝印实锤）

丝印 `FM11NC 081039JOG` 中 "08" 后的 "1" 实为字母 **I** = I2C 接口版本（1039JOG 为批次码）。

官方公开手册（产品概述级）要点：

- ISO14443-A，106~848kbps；三种通道模式：ISO14443-3 / ISO14443-4(ISO-DEP) / AFE 透传
- 8Kbit EEPROM，用户区 900 字节（双界面共享邮箱）
- IRQ_N 中断输出：**低有效、开漏**
- RF 场能量收集可对外供电（汉朔"手机碰一下唤醒标签"的原理，可复刻）
- 配合主控可实现 NFC Forum **Type 4 Tag**；标签断电时手机仍可经 RF 读 EEPROM
- I2C 从机地址：8bit 写地址 **0xAE**（7bit 0x57）——ATC 固件 `init_nfc()` 已实证可通信

已知复位序列：I2C 0xAE 发 `{0x03, 0xB5, 0xA0}`（见 `Firmware/src/nfc.c`）。
⚠️ 公开手册是产品概述级，寄存器级 I2C 命令表公开渠道没有。路线：刷机后实证探测（首选）、向复旦微/代理索取全本手册、参考已知复位序列。

## 背面触点对照（实测）

| 触点组 | 用途 |
|---|---|
| `SWS RXD VCC TXD NRST GND` | 烧录调试口：刷机用 SWS + GND + 供电（SWS = PA7） |
| `CS CK SDA` | NFC 芯片测试点（FM11NC08 的 SDA/SCL/CS） |
| `TXD RXD DC RST` | 屏/调试复用（DC/RST = 屏驱动信号；TXD = PB1 / RXD = PA0） |

## 引脚全表

引脚全表见仓库内 [ATC_TLSR_Paper/Hanshow_Stellar_MN@_E31H_Pinout.txt](../ATC_TLSR_Paper/Hanshow_Stellar_MN@_E31H_Pinout.txt)（atc1441 实测同族 Stellar 板，Nowa 实测屏驱动部分一致）：

| TLSR8359 引脚 | 功能 |
|---|---|
| PA0 / PB1 | RXD / TXD |
| PA1 | EPD_BUSY |
| PA7 | 蓝色 LED + SWS 调试 |
| PB4 / PB5 / PB6 | EPD_CS / EPD_CLK / EPD_MOSI |
| PC5 | EPD 电源门控（经 MOS 管） |
| PD4 / PD7 | EPD_RESET / EPD_DC |
| PD2 / PD3 | 红色 / 绿色 LED |
| PC0 / PC1 | NFC_SDA / NFC_SCL |
| PC4 / PC6 | NFC_IRQ / NFC_CS |
| ANT(38脚) | 2.4G 天线 |

屏：Nowa-154 玻璃实测 **152×152 黑白红三色**，驱动 IC 走 SSD1675/SSD1680 混合时序（上游 `epd_bwr_154.c` 注释），栅极/源极 RAM 映射有偏移——完整推导见 [screen_adaptation.md](screen_adaptation.md)。

## 公开资料链接

- FCC 备案（含说明书 PDF）：<https://fccid.io/2AHB5-NOWA-154>（PDF 直链需浏览器 UA）
- FM11NC08 官方手册（中文/英文）：复旦微官网 <https://www.fmsh.com> 产品页下载
- TLSR8359 数据手册：Telink 官网 <https://www.telink-semi.com>（B85 系列）
- 上游固件与视频讲解：<https://github.com/atc1441/ATC_TLSR_Paper> ｜ <https://www.youtube.com/watch?v=ANHz7EgWx7k>
