# NFC 参考实现差异核对（2026-10-25）

> 本文是 `nfc.c`（v26 收档态）与 7 个同族芯片开源实现的逐点核对报告。
> 结论分三级：**实证矛盾**（参考源码直接推翻本仓既有定性）、**高嫌疑**（失效机理的
> 新解释，待真机重验）、**小差异**（低风险改进项）。
> 对 `hardware_notes.md` 既有结论的修订标注见 §7。参考仓库已克隆到 `_local/refs/`
> （私有区，不入库；本文引用以上游仓库路径为准）。

## 1. 参考实现一览

| # | 仓库 | 芯片 | 接口 | 角色 |
|---|---|---|---|---|
| R1 | [maotek/hanshow_stellar](https://github.com/maotek/hanshow_stellar) | **FM11NC08I** | I²C | **与本机完全同硬件**（Telink TLSR + 汉朔 ESL + T4T），6 个变体共用同一份 230 行 `src/nfc.c`（含两个 1.54" 变体，md5 一致） |
| R2 | [solokeys/solo2](https://github.com/solokeys/solo2) `components/fm11nc08` | FM11NC08（gen-1 同硅片） | **SPI** | 寄存器枚举/位定义/EEPROM 写 magic 的权威来源 |
| R3 | solokeys/solo2 `components/fm11nt082c` | FM11NT082C（gen-2） | I²C | 与本机同款 I²C 访问形式（2 字节统一编址），掩码/仲裁注释最详 |
| R4 | [hgddingjun/FM11NT082C](https://github.com/hgddingjun/FM11NT082C) | FM11NT082C | I²C | 复旦微官方 demo 的 MTK 内核驱动移植（含中文数据手册 PDF） |
| R5 | [openharmony/vendor_hisilicon](https://github.com/openharmony/vendor_hisilicon) `hispark_pegasus/demo/nfc_demo` | **FM11NC08I** | I²C | **复旦微官方 demo 本体**（Hi3861），`app_demo_config.c` 即 nfc.c 注释里"FM11T4T demo"的真身 |
| R6 | [openharmony/device_soc_hisilicon](https://github.com/openharmony/device_soc_hisilicon) `hi3751v350/.../fm11nt081d` | FM11NT081D | I²C | 海思 BSP，纯 EEPROM 写入器（tag 用法） |
| R7 | [derekfan668/701N_SX](https://github.com/derekfan668/701N_SX) `sdk/apps/common/device/nfc/nfc_fm11nt081d` | FM11NT081D | I²C | 杰理 SDK 驱动，**掩码写入带回读验证**的范例 |
| R8 | [dawidoczek/stellar-etag](https://github.com/dawidoczek/stellar-etag) | FM11NC08/I 同族 | I²C | 上游同族板（Stellar），"复位序列杀 RF"现象的出处 |

R1/R5 与本机同为 **FM11NC08I + I²C**；R1 更是同为汉朔 ESL + Telink 平台。

## 2. 七源共识（全部经本地克隆源码复核，非转述）

以下各条在 R1-R7 中**无一例外**成立，构成本芯片 I²C 协议的"公有知识"：

1. **统一 2 字节大端编址**：EEPROM（0x000-0x3FF）与寄存器窗（0xFFF0-0xFFFF）是同一
   个地址空间，读写都是 `[0xAE][addrHi][addrLo][data...]`。R5 的 `Fm11WriteReg` 就是
   `Fm11ReadEep/Fm11WriteEep` 换个名字（`app_demo_config.c:198-227`）。
2. **寄存器窗 2 字节直写可写，无任何解锁前置**：R1 写 `FIFO_FLUSH(0xFFF1)←0xFF`、
   `TXEN(0xFFF4)←0x55`（`nfc.c:197,76`）；R3 `write_reg` 掩码直写
   （`lib.rs:254-266`）；R7 `MAIN_IRQ_MASK←0x54 / FIFO_IRQ_MASK←0x07 /
   NFC_CFG←0x03` 全部 2 字节直写且**写后回读验证**
   （`nfc_fm11nt081d.c:490-500`）；R4/R5 的 `RF_TXEN←0x55` 同。
3. **掩码位=1 为屏蔽**；solokeys 配方 MainIrqMask=0xE5（只放行 RxDone|TxDone|Fifo）、
   FifoIrqMask=0xF5；杰理用 0x54/0x07（放行 Active|RxDone|Arbit）。值可不同，写法相同。
4. **FIFO 经 0xFFF0 突发读写**：读=连续 POP、写=连续 PUSH，不自增到相邻寄存器
   （R1 `nfc.c:220-221,75`；R3 `read_fifo/write_fifo`，作者标 HW-VERIFY 但 R1 实践佐证）。
5. **收帧路径**：RxDone 后读 `FIFO_WORDCNT(0xFFF2)&0x3F` → 0xFFF0 突发读 →
   **去掉尾部 2 字节 RF CRC** → 帧 `[PCB][CLA][INS][P1][P2][LC][data]`，应答回显 PCB。
6. **发帧路径**：写 FIFO → `TXEN←0x55`；长帧按 24/32 字节分块，**每块后重写 0x55**、
   等 `FifoCnt≤8` 再补块（R4 `FM11_RFDataTx`、R5 `app_demo_config.c:404-466`）。
7. **MAIN_IRQ 读清；FIFO_IRQ/AUX_IRQ 子寄存器须补读一次"保证中断拉高"**
   （R4 `fm11nt082c_i2c.c:298-308` 注释原文；R5 `Fm11DataRecv:360-369` 同款）。
8. **EEPROM 页写**：16 字节页、不跨页，STOP 后等 10ms（R5 `EepWritePage` 注释
   "必须延时10ms"；R4 注释"FM441 改 5ms、貌似有问题还是按 10ms 来"）；逐字节查 ACK
   （R4 `sEE_WritePage` 注释：不查会把擦写期 SAK 错误当成功）。CRC8：init 0xFF、
   poly 0xB8 反射，覆盖 UID 9B + ATQA/SAK 4B，存 0x3BB（改 SAK 必重算）。
9. **配置字与模式**：gen-2（NT082C）的 USER_CFG 在 EEPROM 0x390（4B，
   CHK=!(C0^C1^C2)），通道模式 {91 82 21 CD} / tag 模式 {90 82 21 CC}（R4
   `FM11_Init:370-372`）；活寄存器 0xFFE0-2 可临时改、软复位 `0xFFE6←0x55`（≥5ms）
   重读 EEPROM 生效（R3 `configure/enter_nc_iso14443_4`）。**仲裁 CFG2[5:4]=01
   (FCFS)**：10=contact-first 会饿死 RF（R3 注释原话）。
10. **0x3B5（TB_Control）是模式敏感字节**：R1 只做一件事——读 0x3B5，≠0xA0 才补写
    0xA0 + 等 10ms（`nfc.c:186-195`），注释明确"汉朔出厂 0xA0；另一块参考板的 0x90
    会让这副天线/标签配置无法稳定枚举"；R5/R6 写 {T0=0x78@0x3B1, TB=0x90@0x3B5}。
11. **初始化不需要写掩码也能工作**：R1 完全不写掩码、不写 0xFFE0 区、不复位——
    出厂配置 + "IRQ 低 → 读 MAIN_IRQ → RxDone → 收发"即完成 T4T 交互
    （`nfc.c:204-230`）。R7 则是"写掩码 + 回读验证"派。

## 3. 本固件（nfc.c v26）与共识的逐项背离

| 项 | 参考共识 | 本固件现状 | 位置 |
|---|---|---|---|
| 寄存器写通道 | 2 字节统一编址直写 | v24.2 判"2 字节写一律 NAK"后改走 **1 字节"寄存器文件"寻址** `[0xAE][reg][data]` | `nfc.c:348-408,519-529` |
| FIFO 访问 | 0xFFF0 突发 POP/PUSH | 经 1 字节通道 reg0 自增（`cmd_read/cmd_write(0x00,...)`） | `nfc.c:150,228,587,736` |
| 掩码布防 | 2 字节直写 + 回读验证（R7） | 1 字节通道写 reg10/11，无回读；实测从未生效（MIM 恒 0x42） | `nfc.c:519-529` |
| 中断纪律 | IRQ 低 → 读 MAIN_IRQ（读清）+ 子寄存器补读；或掩码只放行 RxDone 后 IRQ 低=帧完整 | 假定掩码已生效（"IRQ 低=RxDone/TxDone/Fifo 事件"），但掩码从未写进去 | `nfc.c:514-518,707-752` |
| CSN 纪律 | R1 每次 handler 进出拉低/拉高，手机会话存活 | v7 教训"CSN 翻转掐死手机会话"→会话期死握 CSN 低 | `nfc.c:677-679` 注释 |
| CC 内容 | R1 用 MLe/MLc=0x3B/0x34；R4/R5 用 0xFF（Hi3861 注释：FF 会分帧、F6 不会） | MLe/MLc=0x00FF（与官方 demo 相同，未验证本机天线/手机组合） | `nfc.c:133-134` |
| 长响应分块 | 24/32B 分块 + 每块 TxEn 0x55 | 一次写完只写一次 TxEn（短应答场景暂无碍） | `nfc.c:228-238` |
| EEPROM 写 ACK | 逐字节查 ACK | 有界等待只查 busy 超时，不查 NAK | `nfc.c:325-346` |
| 配置生效序列 | 写后回读验证（R7）或干脆不写（R1） | `nfc_logger_init` 的 FFE0-2/FFE6 写全部不检查返回值 | `nfc.c:1059-1082` |
| 上电 FIFO 清理 | R1 init 即 `FIFO_FLUSH←0xFF` | 无（v24 后全部写操作移除） | — |

## 4. 问题清单

### 实证矛盾级（推翻既有定性）

**P1 "I-block 出厂门控"终局 vs 同硬件反例。**
v19-v26 定性（hardware_notes「最终定性」）：I-block 无人应答无人转发，通路受出厂
人格（OTP/掩膜）门控。但 R1 在**同芯片同平台同生态位**（汉朔 ESL、TLSR、FM11NC08I、
T4T over I²C FIFO）上用 230 行驱动完成了 I-block 收发，且不写掩码、不写配置、不复位。
两者不能同时为真，除非：汉朔 Nowa 线与 Stellar 线的出厂人格不同（R1 覆盖 2.13"/1.54"/4.2"
Stellar 系，本机是 Nowa 系）。**该定性降级为存疑，需 A/B 实验裁决**（§6 实验一）。

**P2 "1 字节寄存器文件寻址"通道大概率不存在。**
该通道的全部依据是"毒理取证实锤"（见 P3）。而在 2 字节统一编址协议里，
`[0xAE][0x0A][0xE5]` 实为向 EEPROM 0x0AE5 写（10 位地址越界 → 静默忽略/NAK）——
与"掩码写入即刻消失（MIM 恒 0x42）"完全自洽；`[0xAE][0x00][resp...]`（"FIFO 写"）
实为向 0x00xx（UID 保护区）写 → 被拒；`cmd_read(0x00)`（"FIFO 弹出"）实为从 0x0000
起读 EEPROM——v25 期间"FIFO 读出即刻消失/内容怪异"的另一解释。R2 的 SPI 命令字节
（0x20|reg / 0x80 / 0xA0）是 **SPI 接口的协议**，R3 明确 I²C 上没有命令字节。
现固件的 FIFO/掩码/部分 TxEn 路径全部架在这条可疑通道上。

**P3 `{0x03,0xB5,0xA0}`"毒理"的新解读——它恰是 R1 要求的合法配置写。**
既有两个互相矛盾的说法并存于仓库：hardware_notes L50-52（"= 向 0x3B5 写 0xA0，
幂等无害"）与 L96-100（"= 1 字节寄存器译码 NfcStatus←0xB5 + TxEn←0xA0 锁存，RF 变聋
跨软重启存活"）。核对结果：线上字节流 `[0xAE][03][B5][A0]` 与"2 字节地址写
0x3B5←0xA0"**逐位相同**（这正是 R1 init 的核心动作、且 R1 注释说必须保持 0xA0）。
"跨软重启存活"恰恰是 EEPROM 写的特征而非易失寄存器的特征。R8（stellar-etag）的
"杀 RF"有更简单的解释：其发送无 10ms 写周期保护（R5 明言必须延时）、或其板批次
0x3B5 出厂值不同。**"1 字节译码毒理"失去理论支撑，L96-100 应降级为待重验。**

### 高嫌疑级（失效机理的新解释）

**P4 掩码未生效 → v24 会话纪律的地基失效 → 自扰从未排除。**
v24 纪律"只在 IRQ 低时碰总线"的前提是掩码已把 IRQ 低收窄为 RxDone/TxDone/Fifo
（=帧已完整）。但掩码从未写进去（P2 解释了写法错误）：出厂 MIM=0x42（位=1 屏蔽）
下 RxStart/RfPon/Aux 均放行，**场一起来 IRQ 即拉低，固件在 RATS/I-block 接收中途**
就碰总线读 0xFFF7/0xFFF2/0xFFF3。solokeys 源码注释（R3 `arm_interrupts`）逐字警告
这种自毁模式："Reading MAIN_IRQ mid-reception resets the contactless RX via
contact/contactless arbitration ... poll *during* RX and self-reset in a tight loop
(I-block never received)"。v23 的排除论证（"RxDone 亮=帧完整收到，非干扰"）有漏洞：
亮的 RxDone 可能是硬件自动应答的 RATS/防冲突帧，**其后的 I-block 才是被接收中途
轮询打死的对象**（没有 RxDone、FIFO 恒 0——与观察完全一致）。若 P4 成立，则
"芯片不自应答 NDEF"也可能只是 I-block 从未完整到达（T4T 应用都没选上）。

**P5 "IRQ_N 上电常低陷阱"的锁存解释。**
既有归因：出厂 FIM=0x00（一位不屏）+ FIFO-Empty 常真 → IRQ 常低。但按 R4 的中断
结构，FIFO 子中断要走 MAIN_IRQ 的 Fifo 汇总位，而出厂 MIM=0x42 的 bit1=1 恰是
Fifo **被屏蔽**——两条记录自相矛盾（hardware_notes L81 的括号释义"0x42（Fifo+Active
路由 IRQ_N）"也与"位=1 屏蔽"语义相反）。R4/R5 给出第三种解释：**子寄存器
（FIFO_IRQ/AUX_IRQ）的锁存位不补读一次，IRQ 就持续拉低**；R1 则在 init 就
`FIFO_FLUSH←0xFF` 清场。v24.2 观察到的"上电常低"更可能是锁存位未读清或 FIFO 残留，
而非 Empty 常真。重验方法见 §6 实验三。

**P6 v20 的"成功记录"被静默撤回，方向恰与 P1/P4 一致。**
记忆与 `nfc.c:126-128` 注释记载：v20 曾"经 BLE 控制台写 0x3F9 + 软复位翻转仲裁后，
I-block 进 FIFO，本应答器即刻生效"。但 v19-v26 终局又定为"无人转发"，且"尾块
0x3F0-0x3FF 写保护"与"0x3F9 写入成功"两条记录冲突。若 v20 成功为真，则通路未被
门控（支持 P4/P1 方向）；若为误读，需知道错在哪。**建议回查 v20 会话的日志页与
BLE 控制台操作记录。**

### 小差异级

- **P7** CC 的 MLe/MLc=0x00FF 在部分手机上触发 chaining 分帧（R5 注释：FF 分帧、
  F6 不分帧；R1 直接用 0x3B/0x34）。真机应答器启用时应先对齐 R1 的取值。
- **P8** 长响应须 24 字节分块 + 每块重写 TxEn 0x55 + `FifoCnt≤8` 补块（§2.6），
  否则 >32B 的 NDEF 应答会丢。
- **P9** EEPROM/寄存器写应逐字节查 ACK（R4 注释：擦写期 SAK 错误会被当成功）；
  掩码等关键寄存器写后应回读验证（R7 范式）。本固件两者皆无。
- **P10** `nfc_logger_init` 的 0xFFE6/0xFFE0-2 配置段写全不检查返回值，若本机
  该窗确为只读，这段序列一直在空转而无人知晓。
- **P11** CSN 纪律：R1 每次 handler 进出都拉低/拉高（接触侧断电）而手机会话存活，
  与 v7"CSN 翻转掐死会话"教训相抵。可能解释：v7 翻转发生在帧交换中途、或当时芯片
  处于楔死态。R1 模式下 CSN 高电平时间远大于低电平，接触侧多数时间断电——这也
  兼顾了"接触接口 60s 休眠"对抗与省电。

## 5. 两个竞争假设

| | 假设 A：自扰说（P4） | 假设 B：门控说（既有终局） |
|---|---|---|
| 内容 | 通路本来通；v10-v26 的轮询在接收中途碰总线，经仲裁把每个 I-block 打死；掩码写法错误（P2）使 v24 纪律从未生效 | 本机出厂人格（OTP/掩膜）锁死 I-block 转发；Nowa 与 Stellar 产线人格不同 |
| 与证据相容性 | "RxDone 亮 FIFO 恒 0"（亮的 RxDone=RATS）；v20 成功记录；R1 同硬件可行 | "两通道掩码都写不进"；v26 全套人格移植零变化；R1 与本机非同产线 |
| 弱点 | 须解释 v23"干净会话"里 RxDone 后我们已等 IRQ 低才读——但 RxStart 也在放行清单里，此保护本就不存在 | 依赖"产线人格不同"这一未证实假设；与 2 字节写 NAK 的机理无解释 |

判据：§6 实验一一次定音；实验二三四分别独立检验 P4/P5/P6。

## 6. v27 实验设计（仅设计，不执行）

> 原则：每次只动一个变量；沿用既有加固（retention 引脚重建、有界 I²C、楔死恢复、
> EPD 刷新 hold），只换 NFC 协议核心。

**实验一（裁决 A/B）：maotek 最小驱动旁路。**
编译开关 `NFC_MAOTEK_MODE`，与现行 logger 互斥：
1. init：引脚配置同现状 → CSN 低 + 100us → 读 0x3B5，≠0xA0 则写 0xA0+10ms →
   `FIFO_FLUSH←0xFF`（全部 2 字节编址）→ CSN 高。不写掩码、不写 0xFFE0 区。
2. 主循环（main_loop 高频调用）：IRQ 高直接返回；IRQ 低 → CSN 低+100us → 读
   0xFFF7 → RxDone 位 → cnt=0xFFF2&0x3F → cnt∈(2,64] 时 0xFFF0 突发读 cnt 字节 →
   去 2B CRC → 复用现有 T4T 状态机应答（FIFO 写 + TxEn 0x55，改走 0xFFF0/0xFFF4
   2 字节编址）→ CSN 高。
3. 判据：手机 NDEF 应用（NFC Tools/标签助手）读出 CC 与 NDEF 文本 → **假设 A
   成立**，既有"门控"终局作废；连续多场 RxDone 从不亮或 FIFO 恒 0 且 0x3B5 dump
   与 R1 板不同 → 转假设 B 检验（dump 0x390/0x3B0-0x3BF/尾块 与 R1 出厂值对比）。
4. 日志：沿用 0x41000 抓取页格式，多记每场的 0x3B5/0xFFF1-0xFFFD 快照。

**实验二（P4）：掩码 2 字节写三态矩阵 + 回读验证（R7 范式）。**
在 ①空闲无场 ②场存在（手机贴近）③软复位 0xFFE6←0x55+5ms 后 三态下，经 BLE 控制台
`regwrite 0xFFFA 0xE5`（2 字节通道），立即回读 + 1s 后回读 + 场撤后回读。任何一态
写入成功 → "寄存器窗只读"结论作废，v24 纪律可以真正成立；全败 → 支持 B。

**实验三（P5）：IRQ 常低归因。**
上电后按序：读 0xFFF7 → 看 IRQ 是否释放；未释放再读 0xFFF8、0xFFF9 → 看 IRQ；
仍未释放则 `FIFO_FLUSH←0xFF` → 看 IRQ。判定常低源是锁存位、FIFO 残留还是 Empty 常真。

**实验四（P6）：v20 成功复现。**
BLE 控制台 `eeread 0x3F0 16`（尾块现状）→ `eewrite 0x3F9`（仲裁字节，参照 gen-2
CFG2 FCFS=01 的位型）→ `eereset`（软复位重读）→ 读回 0xFFE0/1/2 验证是否翻转 →
手机实测 I-block 是否进 FIFO。全程逐字节 ACK 检查 + 写后 10ms。若尾块确属写保护，
记录 NAK 形态，v20 记录定性为误读。

## 7. 对 hardware_notes.md 的修订索引

| 既有结论 | 位置 | 修订 |
|---|---|---|
| "I-block 数据通路受出厂人格门控"终局 | L59-70 | 降级为假设 B（存疑），指向本文 §5/§6 实验一 |
| 踩坑三：复位序列 1 字节译码毒理 | L96-100 | 与 L50-52 自相矛盾；毒理解读待重验，指向本文 P3 |
| "寄存器窗 2 字节只读 / 1 字节文件通道" | L114-118 | 与 R1-R7 全部矛盾；1 字节通道疑不存在，指向本文 P2 |
| "IRQ 常低 = FIM=0 + Empty 常真" | L119-122 | 增补锁存读清解释与 MIM=0x42 语义矛盾注记，指向本文 P5 |
| 社区参考列表 | L138-143 | 增补 R1/R5/R7（本次新增的三个关键参考） |

## 8. v27-v32 真机实验结果（2026-10-25 晚，实验一~四全部执行完毕）

实验链：v27=maotek 旁路原样 → v28=IRQ 消抖 8ms → v29=盲探针 → v30=盲应答状态机
→ v31=抗重传+兜底 → v32=CC 3B/34 + FIFO 端口自检。日志样本：仓库根
`v27_tap1.bin` ~ `v32_tap1.bin`。

### 裁决结果（两个假设都对了一半）

1. **自扰确证（P4 成立）**：v27 原样移植 maotek 轮询（IRQ 低立即碰总线），I-block
   表现为 RxStart 亮、RxDone 永不出现——接收中途的接触侧读经仲裁杀死非接触接收，
   与 solokeys 注释的自毁循环完全一致。v10-v26 十九轮"I-block 无人转发"的观察
   **全部被此干扰遮蔽**。v28 消抖 8ms 后 RxDone 立即出现。
2. **门控确证（假设 B 成立，且精确到单向）**：消抖后 I-block 完整接收（RxStart+
   RxDone 同帧），但 FifoCnt 恒 0，v29 盲弹 24B FIFO 口只见确定性填充字节
   （0xC0，随芯片状态漂移为 0xC7/0x6A），无任何帧内容——**RF→接触 FIFO 转发被
   出厂配置锁死**（嫌疑最大：FFE2 仲裁=00，源自写保护的尾块 0x3F9）。
   "FifoCnt 镜像说谎"假说同时被排除（计数为 0 是真话）。
3. **TX 通路是开的（重大新发现）**：v30 起盲应答（FIFO 写 + TxEn←0x55，全 2 字节
   编址）能到达手机——手机从"一两帧放弃"变为连续消费应答（3 秒 23 帧），
   NFC Tools/标签助手从"无反应"变为"稳定连卡读出技术信息"。**门控是单向的
   （RX 转发锁、TX 开放）**。
4. **v32 FIFO 端口自检**：写 8B 图案再读回不返回图案（0x6A 填充）——本芯片 0xFFF0
   读端口与参考实现行为不符（写入能进、读出拿不到内容），RX 内容双重不可得。

### 盲应答 T4T 的现状与极限

- 状态机（SELECT→SELECT CC→读 CC→SELECT 文件→读 NLEN→读数据）能走通到数据帧
  （v31 日志：st=5 应答发出完整 NDEF 文本 `nowa154-NFC`，手机继续连发 6 帧）。
- 但 App 最终仍不显示 NDEF。残余障碍（无 RX 内容，纯黑盒）：各家读卡器帧序列/
  FCI 预期差异（MIUI 收到第二个 9000 即重启场）；射频偶发 CRC 误码（AuxIrq 错误位
  实录）；重传与场重启导致的状态机复位竞争。继续迭代需按读卡器逐家调，性价比低。
- **已稳定可用的现实能力**：场检测（IRQ）+ 接触侧 EEPROM 邮箱 + 盲应答保持
  ISO-DEP 会话活跃（App 稳定连卡、可触发读卡器特定行为——如做"碰一下"事件源）。

### 下一步（按性价比排序）

1. **复旦微用户手册**（解锁的正道）。带着今晚的精确定量问题去问：
   a. gen-1 NC08I 的 I-block→接触 FIFO 转发使能条件（我们的 RxDone 亮而 FIFO 恒空，
      仲裁 FFE2=00 改不动：尾块 0x3F0-0x3FF 写保护、0x390 配置区空置无效）；
   b. 0xFFF0 FIFO 口的读语义（写入可出 TX、读回无内容，与 NT082C datasheet
      3.3.3.9 的描述是否不同）；
   c. MainIrqMask/FifoIrqMask 的写使能（2 字节写表现为短暂总线失联后回出厂值）；
   d. 尾块解锁流程。
2. **向 maotek/hanshow_stellar 提问**（GitHub issue）：其板卡 0x3F0-0x3FF 尾块
   dump 与 FFE2 值——若为 01/10 则确认"汉朔 Nowa 产线把仲裁锁死在 00"的产线差异说。
3. 盲应答继续调优（可选）：SELECT 应答带标准 FCI（6F 模板）、按 MIUI 序列适配、
   CRC 误码重试策略。

## 9. 逆向篇：配置空间夺权战役（2026-10-25 深夜，v33 系列）

### 9.1 新知识基座

- **62 页 NT082C 中文手册一直在本地**（`_local/refs/FM11NT082C/FM11NT082C_ds_chs.pdf`，
  hgddingjun 仓库自带，此前只看了 18 页概述版）。关键机制摘录（§引文已核对页码）：
  - **双配置字**（4.3）：实际配置字 @EEPROM Block E4h（NT082C=0x390）
    [CFG0,CFG1,CFG2,CHK=~(C0^C1^C2)]，CHK 过→生效；不过→回退**默认配置字**
    （NT082C @0x3B8-0x3BB）。
  - **USER_CFG2=FFE2**：bits[5:4] 仲裁（00 无优先级邮箱 / 01 FCFS / 10 接触优先 /
    11 非接优先），bits[3:0] **ct_sleep**（CSN 拉高后接触侧延时断电，0.5ms~64ms，
    8-f=常上电）。
  - **寄存器不落电**（p49-50）：接触侧断电后寄存器回上电默认。
  - AUX_IRQ bit6=写被拒 bit7=写成功；RfStatus 高半字节=NFC_STATE（0 IDLE/8 通道待
    RATS/A PPS 后/4 HALT…），本机全部历史观测值（04/06/26/a0/a4/a5/a6）均落入此表。
  - I²C 可写页范围 NT082C=00h-3Ch（0x3D0 起硬件锁）。
- 新参考实现 **canokeys/canokey-core**（同寄存器窗量产级，`interfaces/NFC/`）已克隆；
  FM13DT160 116 页全本手册（FMSH 公开的最厚双界面手册）存 `_local/refs/docs/`。

### 9.2 三桩"幽灵实验"曝光（v20 以来控制台从未真正工作过）

v33 系列调试中连环曝光的历史假象——**以下旧结论全部作废**：

| 幽灵 | 真相 | 影响 |
|---|---|---|
| `regwrite`（case 0xC） | ota.c 长度检查 `data_len>=5` 而帧只有 4 字节——**自 v20 起从未执行过一次写**；且旧代码硬编码上报 ret=0 | "掩码 2 字节写 NAK/读回 FF"（§8 控制台表、hardware_notes）全部无效 |
| `eeread` 长度字节（case 9） | 固件读 payload[4]（越界）而非 payload[3]，靠 RX 缓冲陈旧字节"碰巧"工作，长度时对时错（3b0 16 实回 7B 即此） | 长度显示不可信（数据本身地址正确、与工厂 dump 交叉验证一致） |
| 空闲期总线并发 | v26 固件 logger 空闲轮询与控制台事务并发踩踏 I²C | "寄存器写即楔死/写入即刻消失"部分为竞态假象 |

v33f2 修正：case 0xC 改 `>=4`、case 9 改 `payload[3]`、控制台门槛退役（状态机噪声态
误报）、regwrite 加写后 2ms 延时读回（即读必 FF 是写后瞬态）。

### 9.3 配置空间全图（v33 真实执行的实验，全部可复现）

| 目标 | 通道 | 结果 |
|---|---|---|
| 掩码 0xFFFA←E5 | 2 字节写 + 同会话 2/10ms 回读 | **写入成功**（回读 65=E5 去 bit7，RfPon 位不可置）且会话内保持；**CSN 断电（ct_sleep 0.5ms）即回出厂 42**——"写不进"纯属幽灵实验 |
| 实际配置字 0x390←{03,C2,18,26} | 裸写 & 静默包裹（0x33→写→0xCC→0x77→0x55） | EEPROM 写成功（AuxIrq=80）但 **FFE0-2 纹丝不动**——gen-1 不认 0x390 |
| 活寄存器 FFE2←18 | 2 字节写 + 2ms 回读 | **读回 00：FFE0-2 确为只读镜像**（干净实证） |
| 默认配置字槽 0x3B8←{03,C2,18} | 16B 块 RMW | **被拒**（整块原子写被 0x3BB 牵连——0x3B8-0x3BB 与尾块同级保护） |
| NFC_CFG 0xFFFD←03 | 2 字节写 | **写入成功**；**⚠️ 事故：导致手机解不出应答**（App 连不上、场反复重启重试），刷回 v32 后恢复——gen-1 头文件此寄存器名为 **RF_BAUD**，写 03 疑改坏发射波特率/调制；且它并非纯易失（场供电期间不回退）。**红线：gen-1 禁写 0xFFFD** |
| 尾块 0x3F9←10 | （下午，eewrite 路径真实） | 被拒（硬件厂商区） |
| FIFO 口 0xFFF0 | 写 8B 图案读回 | 无回读（填充 0xC0/C7/6A 随状态漂移）——写入可出 TX、读出无内容双确认 |

**终局结论：I-block→接触 FIFO 转发门控无法从接触侧打开。** 所有配置通路
（0x390 忽略 / FFE0-2 只读 / 0x3B8+尾块硬件锁 / RF 侧需算法认证）都已用
真实执行的实验标定。剩余通路：①复旦微手册（问题清单更锐利了）②RF 侧 AUTH
（需 Proxmark 级读卡器硬件）③maotek 板对比。

### 9.4 固件产物（v33 系列，均在 bin/）

`ATC_Paper_nowa154_v33reconsole.bin`（终版 v33f2，md5 e6d346a9）：v32 运行时 +
RE 控制台（cfgwrite/masktest/unstick/regwrite 延时回读/eeread 长度修正）+
poll 加固（5s 挂载上限破 IRQ 钉低死锁、噪声态不盲发 TX）。中途版本：v33(4af78624)
/v33b(dfc8f205)/v33c(9b6b3991)/v33d(68f45e33)/v33e(01cde273)。注意 19:47 一次
**双构建 md5 不一致**（build2 静默损坏，三重建比对后排除）——双构建纪律再次救命。

**⚠️ v33 实验期事故（20:29-20:41）**：向 0xFFFD 写 03（杰理"默认支持-4"配方）导致
手机无法解码我方应答（日志特征：RfPon+Active 每帧重锁、状态机恒 st=0、场循环重启），
刷回 v32（c4b49128）即恢复。教训：**杰理驱动是 NT081D（gen-2），其寄存器配方不可
移植到 NC08I（gen-1）**——gen-1 的 0xFFFD=RF_BAUD。后续 v33 控制台实验照常可用，
但避开 0xFFFD。

## 10. 逆向续篇（2026-10-25 深夜第二场，v34）：事故、终证与复原

v34（`bin/ATC_Paper_nowa154_v34re2.bin`，md5 7afbfe94）= 撤销 stead-attach（回归
嫌疑）+ 新控制台（eesnap 全片快照→flash 0x45000 / dwrite 非 RMW 直写 /
magwrite SPI magic 直译）+ FIFO 单字节逐弹探针。v34 上完成：

### 10.1 EEPROM 损坏事故与复原

- **损伤**：eesnap 发现 0x200-0x3FF 整段变 FF、NDEF 文本 0x018-0x022 被写花——
  时间窗 20:30-20:41（v33f2 回归风暴期）。机理推断：楔死态下盲应答的 FIFO 写被
  从机半解码成"以响应字节为地址的 EEPROM 写"（响应首字节 PCB=02/03 恰是地址高位
  → 写点全落 0x02xx/0x03xx，与损伤区完全吻合），写周期又被 CSN 断电撕裂 → FF。
- **读伪像**：受损区 16 字节块读返回 FF，单字节/短读返回真值（写本身持久正常）。
  诊断 EEPROM 时须用短读交叉验证。
- **复原**：ATQA/SAK@0x3A0（44 00 04 20）、ATS@0x3B0（05 72 01 57 f7 a0 02）、
  NDEF 文本（54-79-B3-8b）经 dwrite 直写全部恢复并断电重连验证 ✓。
  0x200-0x3FF 保持 FF（出厂为全零；芯片以 ROM 兜底运行已证无功能影响，不再耗
  写周期化妆）。终验：手机稳定识别 ✓。

### 10.2 "出厂人格"的最终实证（ROM/掩膜级）

- **EEPROM 配置区全擦除后 FFE0/1/2 仍 = 03/c2/00 纹丝不动**——活配置根本不从
  EEPROM 加载。v20"尾块镜像"理论正式死亡：出厂尾块内容只是恰好等于 ROM 默认值。
  4A5F 仲裁/ct_sleep 等人格参数在 ROM/掩膜里，**任何 EEPROM 状态都改不了**。
- **尾块 0x3F9 地址级保护**（AuxIrq=0xC0=写周期完成但无权限），擦除态同样拒写；
  **SPI magic {CE,55} 的 I²C 直译对尾块无效**（同样 0xC0）。
- 0x3B8 槽：AuxIrq=bit7 受理但读回语义混乱（受损区伪像混合）。
- **接触侧开锁路径正式清零**。剩余通路只有：复旦微手册、RF 侧算法认证（需读卡器
  硬件）、Stellar 板 ROM/EEPROM 人格对比。

### 10.3 遗留正向空间（未做完的）

1. **盲应答调优**：FCI 模板应答、DESELECT(S-block) 处理、CC 声明小文件尺寸简化
   读流程、重传/场重启竞争——目标从"稳定连卡"推进到"App 显示 NDEF"。
2. **NFC TagInfo 抓协议轨迹**：让手机侧黑盒外化，盲调优的眼睛。
3. FIFO 单字节逐弹探针已在 v34 布防（rec[1..12]=逐次单读、rec[13..24]=突发读），
   下次碰卡日志即出裁决。
4. Stellar 价签采购对比 / 世强代索手册（慢变量）。

## 11. 终章（2026-10-25 第三场，v35-v39）：盲应答之死与最终架构定案

第三场带着 TagInfo 上阵，五个版本把 §10.3 的 1/2/3 全部做完，结论是**大反转**：
盲应答从头到尾就是个幻影。

### 11.1 v34 探针判决

TagInfo 会话日志：`rec[1..12]`（12 次独立单字节读）与 `rec[13..24]`（12B 突发读）
返回**同一个字节**，且该字节**逐事件变化**（89/d7/80/be/d5/49/42/0c/da/0d/95/1c/45/5c/8c/1d），
非帧 CRC、非 PCB、非我方应答 CRC（全候选穷举比对，8 字节仅 1 个巧合命中）。
空闲时恒 0x6a。"gen-1 需逐事务弹出"假设死亡：**0xFFF0 读出的不是 FIFO 数据，
是总线锁存残影**；FifoCnt 恒 0，帧从未进入接触侧可见缓冲。

### 11.2 v35-v38：内容无关性——幻影的铁证

- v35：修 NLEN 应答（旧硬编码 `00 0F` 与 st5 数据长度 `00 12` 矛盾——这个 bug 是
  真的，但修了没用，见下）。
- v36：BSM 时间线仪器（每帧 3B 事件 + 重激活/间隔标记落 0x46000 独立区，容量
  1344 事件；同时 RATS 面包屑页不再占用 64 页环形区）。解决了"环形回卷 8-14 次、
  关键帧全被冲掉"的仪器盲区。
- v37：序列外帧（st≥6）从"兜底回完整文件"改为 `6D 00`。
- v38：SELECT 应用应答加 FCI 模板（`6F 09 84 07 D2 76 00 00 85 01 01` + 9000）。
- **三轮内容大改，手机侧行为逐事件零变化**（v36/37/38 的 BSM 时间线走位完全相同：
  ~20 个单帧循环 + 一次 9 帧深走 + 一次 6 帧走 + 数次 3 帧止于 READ CC）。手机行为
  与我方应答内容**完全无关**——"手机消费应答"解释下这不可能成立。
- TagInfo 终判始终如一：IC INFO=复旦微未知 IC、FULL SCAN=IsoDep+NfcA 无 Ndef、
  NDEF 页="No NDEF Data Storage Populated"。

重新解读：所谓"深走"= 手机对同一命令的**重试级联**（R-NAK/重发，17ms 转折周期 =
我方"TX"垃圾帧被秒速否决）；v30"手机消费 23 帧"实为 23 次重试；"App 稳定连卡"
= RATS/ATS 硬件应答路径（v28 消抖修复所恢复，与盲应答无关）。

### 11.3 v39 自环终审：TX FIFO 写入从未成立

`fifotest`（BLE 控制台 0x13）：写 3B 图案 {5A A5 3C}（不发 TxEn），看 FifoCnt 与弹出：

| 写法 | 写前 cnt | 写后 cnt | 弹出 | 判决 |
|---|---|---|---|---|
| mode 0 内存窗 0xFFF0（v27+ 现行） | 00 | 00 | 6a×6（锁存常数） | 未进 FIFO |
| mode 1 命令通道 0x80（solokeys SPI 直译） | 00 | 00 | 6a×6 | 未进 FIFO |
| mode 2 命令通道 0x00（v25 旧路径） | — | — | 全 FF | 无效命令，楔死（unstick 已复原） |

**接触侧 FIFO 数据通路双向死**：写不进（TX）、读不出（RX，§11.1）、计数器恒 0。
EEPROM 复核：尾块 0x3FA-FF 完好（`c2 03 00 88 d8 27`）——当晚 6 次伪 FIFO 写被
尾块地址级保护挡下，零损伤。同时定案 EEPROM 事故机理：**内存窗 0xFFF0 写在从机
侧按 EEPROM 地址空间译码（0xFFF0 & 0x3FF = 0x3F0 尾块），楔死态译码漂移才落进
0x2xx/0x3xx** ——与 §10.1 损伤区完全互证。

### 11.4 最终架构（出厂人格全景）

- **RF 侧**：防冲突 + RATS/ATS 硬件自答（配置取自 ROM，UID/ATS 布局非线性——
  EEPROM 的 UID/ATS 字节与实发值有固定跳字节映射）。I-block：收得到（RxDone 置位）
  但**永不应答、永不转发**——对手机而言是"只回 ATS 的聋哑 T4T"，TagInfo 判
  "Unknown IC implementing ISO/IEC 14443-4" 完全吻合。
- **接触侧**：EEPROM 信箱（0x00C-0x3EF 可读写，尾块/UID 区保护）+ IRQ/状态遥测
  （MainIrq 读清、RfStatus、FFE0-2 只读、掩码易失可写）。**FIFO 窗残废**。
- **MCU 应答器路线彻底关闭**：RX 门控在 FIFO 上游，TX 门控在 FIFO 写入，无任何
  接触侧绕行。v18-v38 所有"应答进展"观测均为幻影（消费假设误读）。

### 11.5 剩余空间（更新）

接触侧 RE 就此**全面收官**。外部通路不变：复旦微/世强索要 FM11NC08I 手册、
RF 侧 AUTH 算法认证（需读卡器）、Stellar 板人格对比、（新增）向 maotek/
hanshow_stellar 提 issue 求证其板卡 0xFFF0 写是否真进 FIFO——若证实则说明
复旦微按客户掩膜定制人格，本机属"仅信箱"档。

可选对照实验（未做，10 分钟）：v40 把 `nfc_maotek_send()` 整体禁用再碰——若时间线
仍逐事件相同则连"TxEn 发垃圾帧"的微扰都不存在；若节奏放慢到 FWT（~309ms）则说明
TxEn 在发空帧垃圾。不改变战略结论，仅供机理收藏。

### 11.6 本场产物

| 版本 | md5 | 内容 |
|---|---|---|
| v35ndef | 63a37de6 | NLEN 真值 + 探针撤出热路径 |
| v36bsm | 34df681f | BSM 时间线仪器（0x46000） |
| v37tail | cf121146 | 序列外帧 6D 00 |
| v38fci | 6b8e2147 | SELECT 应答 FCI 模板 |
| v39fifotest | cb210228 | FIFO 自环控制台（fifotest 0/1/2） |

工具：`tools/nfc_log_parse.py <dump> [--t4t|--bsm]`（BSM/环形日志解析），
`ble_ota_flash.py fifotest <mode>`。

## 12. 续章（2026-10-26 深夜，阶段 0 系列）：原厂价签反转 + Handy Link 定性

新资源：用户手边共 3 块同款价签——tag#1（本机，v40/v41 实验台）、**tag#2/#3（原厂固件，
#2 永保纯净参照）**；汉朔官方 App Handy Link（com.hanshow.handylink 3.0.1）APK。

### 12.1 阶段 0 判决链（假设树逐层坍缩）

| 实验 | 结果 | 判决 |
|---|---|---|
| 0.1 原厂价签 TagInfo | **NDEF T4T 完整可读**：Text=条码 54-8F-95-8B、CC=`00 0F 20 00 12 00 18 04 06 E1 04 03 84 00 00`（MLe/MLc=18/24、E104、900B）、UID/ATS/ATQA/SAK 与 tag#1 同构（ATS 同 05 72 f7 a0 02）、尾部 `read error [No response]` | **§11.4 "人格在 ROM/掩膜、T4T 聋哑"对原厂态不成立**（对 tag#1 现态仍成立）；同产线可答 ⇒ 非掩膜 |
| 0.4 抠电池（#2/#3） | 读不出、无反应；装回恢复 | 无 MCU 无应答 ⇒ 应答者需主机；混淆变量=断电后 CSN 悬空 |
| 0.5 tag#1 抠电池 | 同样无反应 | 对照组一致 |
| 0.7 v41mute（tag#1，CSN 常低+零 I²C 流量） | 仍读不出（仅系统"有卡"提示），mutemode 0 恢复 | **"接触侧纪律假象"死亡**：v22 常上电旧排除虽已被 v27 自扰发现作废，但真静默条件也不自答 ⇒ tag#1 芯片本态不自应答 |

**假设树收敛为二选一**：① 原厂固件开机发送易失 FIFO 使能序列（over-FIFO 应答，maotek
同款形态，尾部 `[No response]` 像极简应答器通病）② 产线逐台配置差异（尾块 0x3F9/FFE2）。
免 SWS 判别器 = **0.4b**：tag#2 抠电池 + 背面 `CS` 测试点桥 GND（接触侧上电、无主机）碰卡
——读出 ⇒ ②（本机接触侧无解）；读不出 ⇒ ①（后备方案 tag#3 SWS 读回是钥匙，
**用户已定：暂缓、勿催**）。

### 12.2 Handy Link 3.0.1 静态逆向（jadx + libapp.so 字符串）

报告全文 `docs/nfc_handylink_protocol.md`。要点：Flutter+MSAL 企业登录、云端
*.hanshowcloud.net；**NFC 层=标准 NDEF 读（flutter nfc_manager 插件），全库无
D2760000850101/无自定义 APDU**——NFC 只做身份（条码）读取，数据走店内 AP（2.4G 私有/
Wi-Fi）+云，无 BLE 专有协议。**"手机发帧重放"路退役**；攻坚点=让本机像原厂那样服务 T4T。

### 12.3 固件产物（双构建 md5 一致）

| 版本 | md5 | 内容 |
|---|---|---|
| v40vendor | b4a6edd839e85fa46d85dba8bf87c3e6 | txdis 开关（§11.5 十分钟项）+ BSM 0xF3(TxDone 芯片自发射探针)/0xF4(Arbit) + vendorseq 厂商序列回放（8B 记录脚本，case 0x14/0x15） |
| v41mute | af9e53090f79da0d6c2c552e4bf12ce9 | = v40 + mutemode（CSN 常低+零 I²C 静默，case 0x16，0.7 实验） |

操作手册：`docs/nfc_v40_playbook.md`（阶段 0 判决表 + 阶段 2 EEPROM↔实发值 A/B 全命令）。



