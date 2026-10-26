# NFC 逆向 v40 操作手册（2026-10-26）

> 配套固件 `bin/ATC_Paper_nowa154_v40vendor.bin`（md5 `b4a6edd839e85fa46d85dba8bf87c3e6`，双构建一致）。
> 战役背景见 [nfc_reference_diff.md](nfc_reference_diff.md) §8-11（v27-v39 终审：FIFO 双向死、人格在 ROM）。
> 本轮新资源：**另两块原厂固件价签**（厂商协议活样本）+ **汉朔官方 App Handy Link**（`com.hanshow.handylink`）。

## v40 新增能力（对 v39 的三点小改）

1. **`txdis <0|1>`** —— TX 全链路禁用开关（RAM 态，重启回 0=照常发）。用途：§11.5 预设对照——TX 禁用后手机侧行为若逐事件不变，则连"TxEn 发空帧垃圾"的微扰都不存在。
2. **BSM 新事件标记**：
   - `0xF3 [TxDone]` —— 本机 TX 从不进 FIFO（v39 终审），此位亮只能来自**芯片自身发射**。这是"ROM 人格对厂商命令自应答"的检测器：Handy Link 发厂商帧时若 0xF3 出现，即为命中。
   - `0xF4 [Arbit]` —— 接触/非接仲裁事件现场记录。
3. **`vendorseq <hexscript>`** —— 厂商接触侧 I²C 序列回放器（Handy Link 逆向 / 原厂固件尸检的产物在此复刻）。脚本 = 8 字节定长记录串（≤30 条）：

   | type | 功能 | 参数 |
   |---|---|---|
   | 0 | EEPROM 写（2 字节地址，写后 10ms） | addr, len≤4, p1..p4 |
   | 1 | 读 ≤4B（结果顺序拼进 notify） | addr, len |
   | 2 | 延时 | p1 = 毫秒 |
   | 3 | 原始帧写（无地址相位，solokeys magic 直译） | len≤4, p1..p4 |
   | 4 | CSN 断电周期 | p1 = 毫秒（默认 20） |
   | 5 | 寄存器窗快写（写后 2ms） | addr, len≤4 |
   | 6 | FIFO flush（0xFFF1←0xFF） | — |

   示例（读 0x3B5 → 写 0x3B0←A0 → 读回）：
   `python tools/ble_ota_flash.py vendorseq "0103b501000000000003b001a00000000103b00100000000"`

⚠️ 红线不变：**禁写 0xFFFD**（gen-1 RF_BAUD，写 03 毁发射）；尾块/UID/0x3B8-0x3BB 硬件保护不可写；实验前必 `eesnap`。

## 刷机与日志拉取（tag#1）

```
python tools/ble_ota_flash.py flash bin/ATC_Paper_nowa154_v40vendor.bin
# 碰卡实验后拉日志：
python tools/ble_ota_flash.py read 46000 2000 bsm_v40.bin     # BSM 时间线（8 扇区）
python tools/ble_ota_flash.py read 41000 1000 ring_v40.bin    # 环形快照区（4 扇区）
python tools/nfc_log_parse.py bsm_v40.bin --bsm
```

实验固件禁 suspend 耗电，**长期放置刷回 `_nfcexp`**。

## 阶段 0：原厂基线对照（零风险，~30 分钟）

原厂 tag#2/#3 **保持原厂固件不动**（#2 永久纯净参照）。

| # | 操作 | 记录点 |
|---|---|---|
| 0.1 | NFC Tools + TagInfo 各碰原厂价签 3 次 | ① NDEF 可读？② 技术栈（IsoDep+NfcA?）③ UID/ATS/SAK 与 tag#1 是否同构 |
| 0.2 | 手机装 Handy Link → 对**原厂价签**走绑定/闪灯流程 | ① NFC 阶段后是否触发 BLE（系统配对提示/扫描）② 价签 LED/屏可见反应 ③ App 卡在哪一步 |
| 0.3 | Handy Link 对 **tag#1（v40）** 走同一流程 | PC 侧拉 BSM 对比 TagInfo 基线；盯 **0xF3** 标记 |

判定门：0.1 若原厂价签 NDEF 可读 → "FIFO 门控在 ROM 掩膜"结论降级，阶段 4（原厂固件尸检）升为最高优先级。

## 阶段 0 补充：抠电池判别实验（2026-10-26 新增，免工具，5 分钟）

阶段 0.1 已有结果：**原厂价签（tag#2/#3）NDEF 可读**（TagInfo 导出：Text 记录=条码号，
CC=`00 0F 20 00 12 00 18 04 06 E1 04 03 84 00 00`，MLe/MLc=18/24，NDEF 文件=E104，
读尾段 `read error [No response]`）。判定门已触发。下一步判别"谁来应答"：

| # | 操作 | 判读 |
|---|---|---|
| 0.4 | **tag#2/#3 抠电池**（后盖可开，纽扣电池可换）→ NFC Tools/TagInfo 碰 | ✅ 已做（2026-10-26）：**读不出、无反应**；装回电池即恢复 ⇒ 无 MCU 时无应答。倾向判定：**应答者=原厂 MCU**（maotek 同款 over-FIFO 形态）。⚠️ 混淆变量：MCU 断电后 CSN 悬空（接触侧状态未知），0.4b/0.7 排除 |
| 0.5 | **tag#1（v40）抠电池碰卡**（对照组） | ✅ 已做：同样读不出 ⇒ 排除"我方固件轮询干扰芯片服务"的简单版（静默版仍待 0.7） |
| 0.6 | 复原电池后再碰 tag#2/#3 | ✅ 已做：恢复正常（NDEF 可读） |
| 0.4b | （可选，5 分钟）tag#2 抠电池 + 镊子/导线把背面 NFC 测试点 **CS 桥到 GND**（= 接触侧上电、无主机）→ 碰卡 | 若读出 = 芯片"接触侧上电即自应答"，与 0.7 联立直接破案；仍读不出 = 主机必须在场应答（over-FIFO），SWS 读回 tag#3 成唯一关键路径 |
| 0.7 | **v41mute 静默测试**：tag#1 刷 `bin/ATC_Paper_nowa154_v41mute.bin` → `python tools/ble_ota_flash.py mutemode 1`（CSN 常低+总线零流量）→ TagInfo 碰 → 完成后 `mutemode 0` | ✅ 已做（2026-10-26）：**读不出，只有系统"有卡"提示**（防冲突+RATS 层照常），`mutemode 0` 恢复 ⇒ "芯片接触侧上电且静默时自应答"假设**死亡**。tag#1 芯片本态不自应答。**假设树收敛**：应答者=原厂 MCU（over-FIFO），且 FIFO 转发由原厂固件开机使能（易失，每次开机刷新）或产线配置差异（尾块 0x3F9/FFE2）——两者都只能靠 **tag#3 尸检台**裁决 |

注：Handy Link 静态逆向已完成（[nfc_handylink_protocol.md](nfc_handylink_protocol.md)）：
App 侧 NFC=标准 NDEF 读，无自定义帧可重放，路径②退役。


## 阶段 2：EEPROM↔实发值 A/B（tag#1，v40 在板）

目标：划清"哪些 RF 可见字节由 EEPROM 服务、哪些是 ROM 死值"（v34 只证了 FFE0-2 配置镜像；0x3A0/0x3B0 区从未做过写入-实发对照）。

每步流程：**改值 → TagInfo 看实发 → 记录 → 恢复出厂值 → 再验**。

```
# 0. 全片备份
python tools/ble_ota_flash.py eesnap 0 1024
python tools/ble_ota_flash.py read 45000 400 eedump_v40_base.bin

# 1. SAK 0x20→0x00
python tools/ble_ota_flash.py dwrite 3a2 00     # → TagInfo 看实发 SAK
python tools/ble_ota_flash.py dwrite 3a2 20     # 恢复

# 2. ATQA 0x4400→0x0400
python tools/ble_ota_flash.py dwrite 3a0 0400
python tools/ble_ota_flash.py dwrite 3a0 4400   # 恢复（出厂 44 00 04 20 @0x3A0）

# 3. ATS 逐字节（出厂 05 72 01 57 f7 a0 02 @0x3B0；已知实发=05 72 f7 a0 02，跳字节映射）
#    先试最无害的尾字节（hist 02→03），再试 TL/T0
python tools/ble_ota_flash.py dwrite 3b6 03
python tools/ble_ota_flash.py dwrite 3b6 02     # 恢复

# 4. TB 字节 0xA0→0x90（R1 注释：0x90 会让另一块参考板无法稳定枚举——短测即回）
python tools/ble_ota_flash.py dwrite 3b5 90
python tools/ble_ota_flash.py dwrite 3b5 a0     # 恢复
```

观察点：
- TagInfo 的 ATS/SAK/ATQA 显示**是否随 EEPROM 变**。变 → EEPROM 服务（可调 FSCI/FWT，甚至 SAK 翻位试 T2T 人格）；不变 → ROM 死值 + 0x3BB CRC8 联动回退（NT082C 手册 CHK 机制的 gen-1 对应行为，本身就是新知识）。
- 改后手机一时识别不出别慌——恢复字节即愈（v34 已证配置区全擦后芯片以 ROM 兜底运行）。
- 全部结束后再 `eesnap` 一次与 base 比对，确认零残留。

## Handy Link APK 提取（PC 侧 jadx 已备好）

本机网络够不到 Google Play / APKPure（阶段 1 静态逆向需要 APK 文件）：

1. 手机侧安装 Handy Link（Play 商店，或手机侧 APKPure 等你能用的渠道）；
2. 用 MT管理器 / APK Extractor（任意现成提取器）把 `com.hanshow.handylink` 导出成 apk；
3. USB（MTP 直接拷文件即可，无需 adb）复制到 `_local/apk/`；
4. 之后 PC 侧自动接手：jadx 反编译 → 全量检索 NFC/BLE 协议 → 产出 `docs/nfc_handylink_protocol.md`。

复旦微「NFC 标签助手」同理（若应用商店搜得到）；官网下载中心的 APK 链接已全部 404（2026-10-26 实测）。

## 阶段 4：原厂固件尸检（tag#3，需 SWS 烧录器）

1. 用刷 tag#1 时的 SWS 烧录器把 tag#3 全片 512KB **读回**（只读不擦）存档；
2. PC 侧反汇编 + 常量扫描（0x57/0xAE、0x3B5、0xFFF0-0xFFFD）定位厂商 NFC init/服务代码；
3. 与 maotek 驱动差集 → 差集序列经 `vendorseq` 在 tag#1 复刻 → 终判 FIFO 门控是否主机侧可解；
4. 完成后 tag#3 可刷 v40 变第二台仪器（先 `eesnap` 出厂 EEPROM 与 tag#1 diff）。
