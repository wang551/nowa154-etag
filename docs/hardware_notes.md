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

### 真机探测实证（2026-10-24/25，探测固件系列 + `tools/ble_ota_flash.py`）

探测固件在冷启动时对 NFC 芯片做只读侦查（I2C 全地址扫描 + EEPROM 全 1KB dump），结果落
flash `0x40000`，可经 BLE OTA 通道 cmd4 读回；抓取日志在 `0x41000`。实测结论：

- **I2C 存在性**：7bit 地址 `0x57` 唯一 ACK；**CSN（PC6）拉低才 ACK**（CSN 高电平全地址静默）——
  CSN 是接触侧电源开关，I2C 访问前必须拉低
- **寻址格式**：**2 字节大端地址 + 数据**（`START 0xAE(W) addr_hi addr_lo [RESTART 0xAF(R)]`，
  与一代同族 FM11NT081D 海思驱动一致）；1 字节"寄存器式"寻址返回全零，不成立
- **出厂配置区（0x3A0-0x3B6）——芯片出厂即配成 ISO14443-4（NFC Type 4 Tag）**：
  - `0x3A0: 44 00 04 20` = ATQA `0x4400`、SAK1 `0x04`、SAK2 `0x20`（ISO-DEP 支持）
  - `0x3B0: 05 72 01 57 f7 a0 02` = RATS 应答 TL/T0/NFC_CFG/魔数/TA/TB/TC
    （T0=0x72 → FSDI=7 即 128 字节帧；TB=0xA0；0x3B3 的 `0x57` = 本芯片 I2C 地址作魔数）
- **上游"复位序列"`{0x03,0xB5,0xA0}` 之谜解开**：在 2 字节寻址下 = 向配置区 **0x3B5（TB 字节）写 0xA0**——
  本标签出厂值就是 0xA0，序列只是重写同值（幂等），并非芯片复位；不同批次/型号写坏 TB 才可能杀 RF。
  本固件不发该序列
- **EEPROM 里的出厂 NDEF**（0x0010）：`00 12 d1 01 0e 54 02 65 6e` + `"54-79-B3-8B"` =
  NLEN=18 的 Text 记录（lang=en），内容即本标签的汉朔条码号
- **手机实测（NFC Tools，2026-10-24）**：读出 ISO 14443-4 / IsoDep+NfcA、序列号
  `1D:3B:E2:F3:80:00:00`（=EEPROM UID 逐字节吻合，BCC 被正确剥离）、ATQA 0x0044、SAK 0x20
  （=0x3A0 配置区逐字节吻合）。NFC Tools 把它误显示为 "NXP Mifare Plus" 是其数据库不识复旦微的
  UID 前缀所致；复旦微官方"NFC 标签助手"也曾碰出过同样的纯标签信息页（成功率极低）
- **最终定性（v19-v26 七轮真机实验定案，2026-10-25/26）**：
  - 防冲突 + RATS/ATS 由芯片硬件自动应答（所以任何手机都能"发现有卡"，系统提示即此层）；
  - **其后的 ISO-DEP I-block 无人应答也无人转发**——十几场干净会话实锤：`MainIrq=0xF2`
    （含 RxDone，帧确实收到）而 `FifoWordCnt` 恒 0（接触侧 FIFO 从未收到任何字节），
    芯片也不自应答（无任何 App 读到过 NDEF）。~~早期"tag 自应答"的判断是错的，已撤回~~
    （系统"有 NFC 卡"提示只是防冲突层的通用检测，不是 NDEF 内容）；
  - 已排除的变量：接触侧供电时序（v22 常上电无效）、MCU 轮询干扰（v23 干净数据里
    RxDone 亮=帧完整收到，非干扰）、IRQ 掩码配置（两通道写均被拒，见下）、
    gen-2 配置字/T2T 人格移植（v26 全套写入无任何行为变化）；
  - **最终定性（2026-10-25 v27-v34 全链定案）**：
    ① **自扰为真**——紧轮询在接收中途碰总线、经仲裁杀死每个 I-block，遮蔽了
    v10-v26 全部观察；IRQ 低后消抖 8ms 再服务即修复。② **门控为真且单向**——
    消抖后 I-block 完整接收（RxDone 亮）但从不进接触 FIFO；~~TX 通路开放（盲应答
    可达手机）~~（**②后半已被 v39 撤回**，见下一条）。③ **人格在 ROM/掩膜级
    （v34 终证）**——EEPROM 配置区整段擦除后活配置 FFE0-2 纹丝不动；尾块地址级
    保护（擦除态也拒写）、SPI magic 无效、0x390 被忽略。2026-10-25 晚事故：
    v33f2 风暴期 EEPROM 0x200-0x3FF 被误写损伤（已复原关键区，详见
    nfc_reference_diff.md §10.1）。
  - **最终定性（2026-10-26 v35-v39 终审修正，见 nfc_reference_diff.md §11）**：
    ②后半"TX 通路开放/盲应答可达手机"是**误读幻影**——v35/v37/v38 三轮应答内容
    大改（NLEN 真值/6D00/FCI），手机侧行为（BSM 时间线）逐事件零变化；v39
    `fifotest` 自环终审：**三种 FIFO 写法（内存窗 0xFFF0 / 命令 0x80 / 命令 0x00）
    全都不进 FIFO**（FifoCnt 恒 0，弹出=锁存常数，0x00 命令还会楔死从机）。
    所谓"手机消费应答/稳定连卡"实为重试级联 + RATS/ATS 硬件路径。**FIFO 数据
    通路双向死**：RX 门控在上游、TX 写入根本不落地、计数器恒 0、数据口读数是
    总线锁存残影（空闲 0x6a，RF 事件期逐事件变值）。接触侧只剩 **EEPROM 信箱 +
    IRQ/状态遥测**。MCU 应答器路线就此关闭；0xFFF0 内存窗写实为按 EEPROM 地址
    译码（&0x3FF 落尾块，被地址级保护挡下——与 §10.1 EEPROM 事故机理互证）。
  - **最终定性修正（2026-10-26 深夜阶段 0，见 nfc_reference_diff.md §12）**：原厂固件价签
    （tag#2/#3）TagInfo **NDEF T4T 完整可读**（Text=条码号、CC MLe/MLc=18/24/E104/900B，
    UID/ATS 与本机同构）；抠电池即失联、tag#1 以 v41mute 静默+CSN 常低也不自答 ⇒
    **"ROM 掩膜锁死"仅对本机改固件态成立**——应答者=原厂 MCU（over-FIFO 嫌疑最大）
    或产线逐台配置差异（尾块 0x3F9/FFE2），判别器=0.4b（原厂板抠电池+CS 测试点桥 GND）。
    Handy Link App 逆向结论（标准 NDEF 读、无自定义协议）见 docs/nfc_handylink_protocol.md。
- **UID 块（0x0000）**：`1d 3b e2 | 4c | f3 80 00 00 | 73`，两个防冲突校验位均自洽
  （BCC1 = 88^UID0^1^2 = 0x4C，BCC2 = UID3^…^UID6 = 0x73），UID = `1d:3b:e2:4c:f3:80:00`
  （手机读出的序列号已证实）
- **0xFF00-0xFFFF 寄存器窗（权威命名，三源互证：solokeys fm11nc08 + 复旦微 NT082C 内核驱动
  iic.h + 本机实证）**：`0xFFF0`=FIFO 数据口（读会弹出，仅在 cnt>0 时碰）/ `0xFFF1`=FifoFlush /
  `0xFFF2`=FifoWordCnt / `0xFFF3`=RfStatus（空闲 0x04）/ `0xFFF4`=**RfTxEn（写 0x55 触发非接触口
  发送；写 0x77 切回接收态）** / `0xFFF5`=RfBaud / `0xFFF6`=RfRats（存手机 FSDI）/ `0xFFF7`=MainIrq /
  `0xFFF8-9`=Fifo/AuxIrq / `0xFFFA-C`=三个掩码 / `0xFFFD`=NfcCfg（**⚠️ 禁写：gen-1 此寄存器名 RF_BAUD，
  写 03 会让手机解不出应答且场供电期不自恢复，2026-10-25 事故实锤**）/ `0xFFFE`=VOUT_CFG /
  **`0xFFFF`=EE_WR_CTRL（EEPROM 写控制，本芯片读出 0x01）**。**MainIrq 位表**：bit7=RF_PWON、
  bit6=ACTIVE、bit5=RX_START、bit4=RX_DONE、bit3=TX_DONE、bit2=ARBIT、bit1=FIFO、bit0=AUX；
  出厂 MainIrqMask=0x42（按位=1 屏蔽语义：Fifo 与 Active **被屏蔽**，放行
  RfPon/RxStart/RxDone/TxDone/Arbit/Aux），掩码为活寄存器、场撤即复位回出厂值。
  **AuxIrq bit7 = EEPROM 写完成标志**（写矩阵实证：成功写后=0x80）。批量顺序读在 0xFFF0+ 会
  重复同一值（端口别名伪影），须逐字节读
- **⚠️ 踩坑实录（v2-v5 抓取器三轮误诊）**：开机 ~19s（首次 deep retention）后 I²C 读全 0xFF、
  IRQ 疑似常低——三轮排查（芯片卡死/总线毛刺/CSN 断电重启）均误，真因是 **MCU 侧**：retention
  唤醒后 `gpio_init(0)` 重置数字引脚配置，而 `init_nfc()` 只在冷启动跑，CSN(PC6) 输出使能丢失后
  被 10K 上拉拉高 = 接触侧断电 = 假性"全 0xFF"；IRQ(PC4) 输入使能丢失 = 读数假象。
  **修复 = 每次访问前重建全部引脚配置**（nfc.c `nfc_bus_attach()`），芯片本身从未坏过
  （修复后芯片穿越 150+ 次 retention 唤醒持续健康，v6 真机验证）。教训：SDK 无界 busy-wait
  在死总线上会冻结 main_loop 40s+，I²C 原语必须带超时自建
- **⚠️ 踩坑二（v12-v14）：EPD 分钟刷新 brownout + 接触接口 60s 自动休眠**——ATC 上游每分钟
  重画时钟，EPD 刷新的大电流把 3.3V 轨拉塌，NFC 芯片跟着楔死（v12 日志实锤：每次刷新后
  ~60s 内必然 all-FF）；实验期用 `epd_hold_image=1` 停刷解决。另有接触接口约 60s 无 CSN
  电源周期就休眠（I²C 变死、CSN 脉冲即愈）的机制，固件以"每次轮询 CSN 通断"对抗（v16 起心跳
  全程健康）
- **⚠️ 踩坑三（关键！）：上游"复位序列"`{0x03,0xB5,0xA0}` 的真毒理**——低位地址（0x03B0 区）
  的 I²C 写在一代芯片上走 **1 字节地址 + 自动递增的寄存器译码**（非 2 字节 EEPROM 地址！），
  该序列 = `NfcStatus←0xB5` + **`NfcTxEn←0xA0`（TX 使能锁存）**→ 芯片 RF 变聋且**跨软重启
  存活**（OTA 重启不清易失寄存器）。dawidoczek/stellar-etag 观察到的"杀 RF"即此。本固件已永久
  禁用该序列，并开机用 2 字节寄存器写 `0xFFF4←0x00` 解毒（实证有效，免断电）。
  **⚠️ 2026-10-25 复核：此解读存疑**——线上字节流与"2 字节地址写 0x3B5←0xA0"逐位相同
  （maotek 同硬件驱动恰要求保持 0xA0 不变），"跨软重启存活"更像 EEPROM 而非易失寄存器
  的特征；"1 字节译码通道"本身在全部参考实现中不存在，待重验。见
  [nfc_reference_diff.md](nfc_reference_diff.md) P2/P3
- **0xFFE0-0xFFEF 寄存器区（二代同源，gen-1 为只读镜像）**：`0xFFE0`=USER_CFG0（bit0
  OP_MODE_SELECT，本芯片已=1 即 NC 通道模式）/ `0xFFE1`=USER_CFG1（bits[3:2] NFC_mode=00
  即 ISO14443-4）/ `0xFFE2`=USER_CFG2（bits[5:4] 仲裁：10=contact-first 饿死 RF、01=先到
  先得——**本芯片出厂 0x00（tag 自应答），gen-1 上活写被拒**，改它须写 EEPROM 配置区）/
  `0xFFE6`=RESET_SILENCE（写 0x55=软复位重读 EEPROM，写 0x33=非接静默，写 0xCC=RF 激活）。
- **✅ EEPROM 写已解锁（v18，2026-10-25）**：**普通 2 字节地址 I²C 写 + STOP 后等 10ms
  擦写周期即可，无需任何 magic**（v10 时代"写被拒"的真因 = 写完没等 10ms 就读回）。
  写权限分区（实测）：`0x00C-0x3EF` 可写（16B 块对齐最稳）；**0x000-0x00B（UID）写保护**；
  **0x3F0-0x3FF（尾块/USER_CFG 镜像区）写保护**；**0x3BB（CRC8 校验字节）写保护**。
  写完成标志 = AuxIrq(0xFFF9) bit7=0x80。CRC8 算法（改 SAK/ATQA 必须重算，覆盖
  UID 9B + ATQA/SAK 4B）：初值 0xFF、poly 0xB8 反射（`crc=(crc>>1)^0xb8 if crc&1`）。
  工具：`tools/ble_ota_flash.py eeread/eewrite/eereset/nfcdiag/regwrite/cfgwrite/masktest/unstick`
  （BLE 在线控制台，ota.c case 8-0xF；v33f2 修正了 regwrite 自 v20 不执行、eeread 长度
  越界两个历史 bug，勿用旧固件跑这些命令）
- **寄存器窗读写的真相（2026-10-25 深夜 v33 系列终版，替代 v24-v25 全部旧结论）**：
  旧"2 字节写 NAK / 1 字节通道 / 掩码写不进"结论经查多为**幽灵实验**——ota.c 的
  regwrite 命令自 v20 起因长度检查差一字节**从未真正执行过**，且空闲期 logger 轮询
  与控制台事务并发踩踏总线（详见 nfc_reference_diff.md §9.2）。真实图景：
  **掩码 0xFFFA/B/C 可写**（同会话写 E5 回读 65，bit7 不可置；RfPon 恒放行）但
  **CSN 断电（ct_sleep=0.5ms）即回出厂值**；**FFE0-2 只读**（写 18 回读 00，干净实证）；
  **0xFFFD（NFC_CFG）可写**（易失）；EEPROM 0x390 可写但被芯片忽略（gen-1 不认
  NT082C 的实际配置字机制）；**0x3B8-0x3BB（默认配置字槽+CRC8）与尾块同级硬件锁**
  （整 16B 块原子写被牵连拒绝）。I-block→FIFO 转发门控**无法从接触侧打开**。
- **IRQ_N 布防陷阱（v24.2 定案）**：出厂 FifoIrqMask=0x00（一位不屏）而 FIFO-Empty 位
  常真 → **IRQ_N 从上电起被永久拉低**，任何"IRQ 低=有事件"的逻辑都会被骗成永久会话态
  （禁休眠+密轮询）。掩码语义：位=1 为屏蔽（solokeys 佐证：其 arm_interrupts 写
  MainIrqMask=0xE5、FifoIrqMask=0xF5）。
  **⚠️ 2026-10-25 补记：归因存疑**——出厂 MIM=0x42 的 bit1（Fifo）按"位=1 屏蔽"应不放行
  FIFO 子中断，与"Empty 常真拉低 IRQ"的归因自相矛盾；复旦微驱动的解释是 FIFO_IRQ/
  AUX_IRQ 锁存位须补读一次 IRQ 才释放，且 maotek 驱动 init 即 FIFO_FLUSH 清场、全程
  不写掩码也能正常工作。重验见 nfc_reference_diff.md P5/实验三
- **gen-2 配置字与模式切换（公开源码逆向，本机无效但记录在案）**：FM11NT082C 的
  USER_CFG 配置字在 EEPROM 0x0390（4 字节 `[CFG0,CFG1,CFG2,CHK]`，CHK=!(CFG0^CFG1^CFG2)）；
  复旦微 demo 的"Tag 模式"=`{90 82 21 CC}`（校验和成立）、"通道模式"=`{91 82 21 CD}`；
  OP_MODE=CFG0 bit0（0=NT tag、1=NC 通道）、仲裁=CFG2[5:4]（10=接触优先会饿死 RF、
  01=先到先得）。**本机（gen-1 NC08I）写入这些字节（含全套 T2T 人格移植：CC E1 10 6D 00 +
  NDEF TLV + SAK_Control + 断电重载）行为零变化**——gen-1 的人格与 gen-2 的 EEPROM
  配置机制不同（疑似出厂固化）
- **T4T 应答器（nfc.c 内置，当前待命）**：帧格式按复旦微 demo——FIFO 帧含 PCB 首字节
  （02/03 交替），应答须回显 `resp[0]=rx[0]`，SELECT 按 DF 名(P1=04)/文件 ID(E103/E104)、
  READ/UPDATE BINARY；TX = FIFO 写 + TxEn←0x55。**数据通路解锁（芯片人格或厂商资料到手）
  后即自动生效**
- 用户区 0x0010-0x0393 除 NDEF 外全零；`0x3FA-0x3FF = c2 03 00 88 d8 27`（尾部厂商/校准块，
  0x3FA/0x3FB=c2/03 与 FFE1/FFE0 镜像值对应，整块写保护）

公开渠道没有寄存器级手册（Reddit 也有人求而不得），社区参考：solokeys/solo2 与
Nitrokey/nitrokey-3-firmware 的 `components/fm11nc08/`（**同型号 SPI 版完整驱动**，含
EEPROM 写 magic/校验、寄存器枚举、IRQ 位定义）；hgddingjun/FM11NT082C（**复旦微 demo 移植的
MTK 内核 I²C 驱动**，EEPROM 读写/寄存器/T4T 应答/Tag-Transceive 模式切换全流程，是 I²C 侧
最权威的公开参照）；同款汉朔板 dawidoczek/stellar-etag 的 `nfc.c` 实证复位序列在某些芯片上
会破坏 RF。

2026-10-25 新增三个关键参考（逐点对照见 [nfc_reference_diff.md](nfc_reference_diff.md)）：
**maotek/hanshow_stellar**（**与本机完全同硬件**：Telink + FM11NC08I + 汉朔 ESL，
230 行最小 T4T 驱动，不写掩码、不复位即可收发 I-block）；OpenHarmony Hi3861
`nfc_demo`（**FM11NC08I 官方 demo 本体**，vendor_hisilicon 仓库
hispark_pegasus/demo/nfc_demo）；杰理 SDK `nfc_fm11nt081d`（掩码写入后回读验证的
范式）。以上均已克隆到 `_local/refs/` 备查。

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
