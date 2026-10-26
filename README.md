# nowa154-etag — 汉朔 Nowa-154R-N 电子价签开源固件

> **English TL;DR** — Open firmware adaptation for the Hanshow **Nowa-154R-N** e-paper
> shelf label (1.54" 152×152 black/white/**red** + NFC + BLE, Telink TLSR8359).
> It is a fork of [atc1441/ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper)
> with a properly reverse-engineered 152×152 panel driver (gate window 144..295,
> source offset +8), two new BLE commands (image hold, red plane), a fix for an
> upstream BLE advertising bug, and a single-file web upload tool with dithering,
> red-channel extraction and verified BLE OTA (two-step flash confirmation). Flash over UART with a web flasher — no EVK needed.

把闲置换下来的汉朔电子价签刷成**可自由传图的 BLE 三色墨水屏标签**：
自定义图片/时钟/温湿度显示，黑白红三色，手机浏览器直连传图。

本固件专门适配 **Nowa-154R-N**（`154` = 1.54 英寸，`R` = 黑白红三色，`N` = 带 NFC）。
其他型号的价签请直接使用上游 [ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper)。

## 主要改动（相对上游）

| 改动 | 说明 |
|---|---|
| **152×152 屏驱动** | 上游 1.54" 驱动按 200×200 几何编写且型号自动探测失败；本 fork 实测反推出玻璃映射：栅极窗口 Y=144..295、源极偏移 δ=8、画布 152×160。推导过程见 [docs/screen_adaptation.md](docs/screen_adaptation.md) |
| **BLE 断线广播恢复修复** | 上游 bug：Telink SDK 连接建立即停广播，断开后从不恢复 → 断连一次标签就从所有扫描里消失，只能抠电池。已在 `ble_disconnect_callback()` 中补调 `app_switch_to_indirect_adv()` |
| **`0x05` 图片锁定命令** | `05 01` 锁定自定义图片（跳过主循环每分钟时钟重画），`05 00` 恢复时钟。易失，重启即回时钟 |
| **`0x06` 红平面命令** | 三色屏红平面全链路：`EPD_Display()` 增加红图参数，BLE 可切换 `0x03` 写入目标平面。旧固件收到 `0x06` 走 default 忽略，安全降级 |
| **顺手修正** | `EPD_*_read_temp()` 返回值改 `int8_t`（负温度不再溢出）；OneBitDisplay 负 x 坐标字符串绘制越界裁剪（`obd.inl`） |
| **网页传图工具** | `web_tools/nowa152_image_upload.html`：单文件零依赖，阈值/Floyd–Steinberg/Bayer 抖动、红像素提取、旋转翻转、内置方向校准测试图案、BLE 直发 |
| **网页 OTA 刷写** | 同一网页工具内置蓝牙刷固件：整暂存区擦除 → 传输 → 设备端字节和校验 → 两步确认后才发刷入命令，任何异常都不触碰运行固件（上游 OTA 页无任何校验，传坏即刷坏）。协议细节见 [docs/ble_protocol.md](docs/ble_protocol.md) |

## 仓库结构

```
├── ATC_TLSR_Paper/        # 固件源码（上游快照 + 本 fork 改动，自带 tc32 工具链）
│   └── Firmware/src/      #   改动集中在 epd*.c/h、app.c、ble.c、epd_ble_service.c
├── web_tools/             # 网页传图 / OTA 工具（单 HTML，Chrome/Edge）
├── bin/                   # 可直接刷写的固件
├── tools/                 # 构建重试、布局预览、BLE OTA/EEPROM 控制台 CLI、NFC 日志解析、SWS 读回（Python）
└── docs/                  # 硬件笔记、屏适配推导、BLE 协议
```

## 快速开始

### 刷机（UART，无需 EVK）

1. 硬件：USB-UART 转接板（CH340 等）。接线：**适配器 TX → 价签 SWS**、GND→GND、纽扣电池供电（接线图见 [ATC_TLSR_Paper/USB_UART_Flashing_connection.jpg](ATC_TLSR_Paper/USB_UART_Flashing_connection.jpg)；SWS 复用在 PA7，烧录时蓝灯闪烁属正常）
2. Chrome 打开网页烧录器：<https://atc1441.github.io/ATC_TLSR_Paper_UART_Flasher.html>
3. 波特率 **460800**，先点 **Unlock** 解锁 flash，再写入 `bin/` 下的固件
4. 原厂固件在深睡：烧录器的“激活延时”设大于广播间隔，多试几次抓唤醒窗口
5. 刷完拔掉 UART 线（TX 挂着会连不上 BLE）

> 芯片 ROM 自带 SWS 下载协议，刷不砖，随时可重刷。
> **建议动手前先整片读出原厂固件存档**（网页烧录器无读片功能，需 Telink EVK 或 tc32 工具链）。

### 传图（BLE）

1. `web_tools/nowa152_image_upload.html` 用 Chrome/Edge 打开（`file://` 被浏览器拦截 BLE 时，`python -m http.server` 后走 `http://localhost:8000`）
2. 拖入/粘贴图片 → 152×152 像素级预览 → 选择填充裁切或完整显示、二值化算法（阈值/FS 抖动/Bayer）、旋转翻转反相
3. 首次建议先发**内置测试图案**核对方向（边框 + 四角 L 形 + 红/黑角标），方向不对用翻转按钮纠正，无需重刷
4. 发送 → 等待刷新完成 → 图片自动锁定不被时钟覆盖；点“恢复时钟”即回到 `05 00`

完整 BLE 命令与像素打包公式见 [docs/ble_protocol.md](docs/ble_protocol.md)。

### 固件 OTA（BLE）

不想接 UART 线时，用同一网页工具的「固件 OTA」区蓝牙刷固件：

1. 连接设备 → 「选择固件 .bin…」（自动校验 Telink 魔数 KNLT、大小上限，算 md5 并与下方版本表比对）
2. 点「① 传输并校验」：只写设备 128KB 暂存区，**不碰运行中的固件**，失败 / 断连可反复重试（约 40 秒）
3. 设备端字节和与本地一致后才解锁「② 确认刷入」：点击后设备擦写自身并重启（约 5~10 秒），**期间切勿断开连接或取出电池**
4. 刷入瞬间断电没有 bootloader 可回滚，只能 UART 救（见上节）

命令行场景（自动化 / 无浏览器）可用 [tools/ble_ota_flash.py](tools/ble_ota_flash.py)：`python tools/ble_ota_flash.py flash <bin>` 刷写、`readback` 读回 NFC 探测结果区，协议与网页工具一致（含 cmd6 校验与守卫复位握手）。

## 固件版本

| 文件 | md5 | 状态 |
|---|---|---|
| `bin/ATC_Paper_nowa154_final.bin` | `ef7456e09df34ead39cc581be6cfa968` | 旧稳定版（黑白传图，已被 `_red` 取代） |
| `bin/ATC_Paper_nowa154_red.bin` | `12943dd8b46d049dbaa6fb8513e50ec4` | 稳定版（真机验证：红通道显示正常 + 传图像素级正确 + 温度符号；显示功能已被 `_v42daily` 收编，保留作回退） |
| `bin/ATC_Paper_nowa154_v42daily.bin` | `59c4b63c9195fa50c7014996660394c7` | ✅ **推荐日常版**（2026-10-26 NFC 收档）：= v41 撤销实验期强制锁屏与常驻抓取器——时钟分钟刷新恢复、suspend 低功耗恢复；NFC 冷启动探测 + BLE 控制台保留，碰卡时手机有"有卡"提示（防冲突 + RATS 由芯片硬件自应答；无 NDEF——数据通路受出厂人格门控）。源码 `NFC_LOGGER=0`，双构建 md5 一致 |
| `bin/ATC_Paper_nowa154_nfcprobe.bin` | `7aeab2d83e52767c93f1333dd84fd6a5` | 早期 NFC 探测版（已被 `_nfcwrite1` 取代） |
| `bin/ATC_Paper_nowa154_nfcprobe6.bin` | `978ecf97b8809f06f340d39055738df9` | NFC 抓取器迭代版（已被 `_nfcexp` 取代） |
| `bin/ATC_Paper_nowa154_nfcwrite1.bin` | `91d723b6e549bd9565482427a9fcf457` | EEPROM 写解锁验证版（裸写+10ms，已被 `_nfcexp` 取代） |
| `bin/ATC_Paper_nowa154_nfcexp.bin` | `a33d29b0a9dcd2df23adc9f9635f689d` | ✅ **长期放置版**（NFC 实验收档）：BLE 在线 EEPROM 控制台（eeread/eewrite/eereset/nfcdiag/regwrite）+ 抓取器 v2 + T4T 应答器（待芯片人格解锁后生效）。NFC 数据通路受芯片出厂人格门控，详见 [docs/hardware_notes.md](docs/hardware_notes.md) |
| `bin/ATC_Paper_nowa154_v40vendor.bin` | `b4a6edd839e85fa46d85dba8bf87c3e6` | 🔬 实验版（v40）：TX 禁用开关（`txdis`）+ BSM TxDone/Arbit 探针（芯片自应答检测器）+ `vendorseq` 厂商序列回放。逆向战役 v27-v39 见 [docs/nfc_reference_diff.md](docs/nfc_reference_diff.md) §8-11，操作手册见 [docs/nfc_v40_playbook.md](docs/nfc_v40_playbook.md)。禁 suspend 耗电，长期放置刷回 `_nfcexp` |
| `bin/ATC_Paper_nowa154_v41mute.bin` | `af9e53090f79da0d6c2c552e4bf12ce9` | 🔬 实验版（v41）= v40 + `mutemode`（CSN 常低 + 零 I²C 流量静默模式，判"接触侧上电且静默时芯片是否自应答 T4T"）。用法见 playbook 阶段 0.7 |

`ATC_TLSR_Paper/Firmware/ATC_Paper.bin` 是当前源码的最新构建（v42daily，与 `bin/` 内同名文件一致）。

## 编译

需要安装 Python。Windows 下进入 `ATC_TLSR_Paper/Firmware/` 运行 `makeit.exe`，Linux 下 `make`（tc32 工具链已随仓库自带）。

一个已知的坑：

1. **make 不跟踪头文件依赖**：改过 `*.h` 后必须先删 `out/` 和 `ATC_Paper.bin` 强制全量重编，否则产物与旧版 md5 完全相同

## 文档

- [docs/hardware_notes.md](docs/hardware_notes.md) — 硬件鉴定：型号解码、TLSR8359、FM11NC08I NFC 芯片、烧录触点与引脚全表
- [docs/screen_adaptation.md](docs/screen_adaptation.md) — 152×152 玻璃映射的逆向推导（四轮刷机实验定出栅极/源极偏移）与像素打包公式
- [docs/ble_protocol.md](docs/ble_protocol.md) — BLE 服务/特征 UUID、命令表（含新增 `0x05`/`0x06`）、发送序列、蓝牙 OTA 安全刷写序列、Windows GATT 排坑
- [docs/nfc_reference_diff.md](docs/nfc_reference_diff.md) — NFC 芯片（FM11NC08I）接触侧逆向战报：七源参考实现逐点核对、配置空间夺权战役（v27–v41）、出厂人格终审
- [docs/nfc_handylink_protocol.md](docs/nfc_handylink_protocol.md) — 汉朔官方 App Handy Link 逆向：NFC 层为标准 NDEF 读、无自定义协议，数据通道走店内 AP + 云
- [docs/nfc_v40_playbook.md](docs/nfc_v40_playbook.md) — v40/v41 实验固件操作手册（原厂对照判别实验、EEPROM↔实发值 A/B、vendorseq 厂商序列回放）

## 已知限制与路线

- [x] ~~红通道真机验证~~（2026-10-21 通过：红色显示正常，红平面映射与黑白一致）
- [x] NFC（FM11NC08I）战役收档（2026-10-26 搁置）：接触侧逆向收官——FIFO 数据通路双向死（RX 门控在上游、TX 写入不落地，v39 自环终审），出厂人格锁定，接触侧仅剩 EEPROM 信箱 + IRQ/状态遥测；Handy Link App 侧仅标准 NDEF 读（无自定义协议可重放）；原厂价签 NDEF 可读 ⇒ 应答者=原厂 MCU（over-FIFO 嫌疑最大）。战报见上列 NFC 文档。日常版碰卡仍有"有卡"提示（防冲突 + RATS 硬件自应答）。重启此线的钥匙 = SWS 读回 tag#3 原厂固件（`tools/pico_sws_*.py` 工具链已备）
- [ ] 屏型号被强制为 BWR154（本 fork 专机专用；多型号需求请用上游）
- [ ] 时钟布局对 152×152 的排版优化（`tools/render.py` 可离线预览布局方案）

## 致谢

- [atc1441](https://github.com/atc1441) — [ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper) 原作者，本项目的基线
- [reece15](https://github.com/reece15) — [stellar-L3N-etag](https://github.com/reece15/stellar-L3N-etag)，同族 2.9" 移植范本与红 LUT 参考
- [Larry Bank](https://github.com/bitbank2) — OneBitDisplay / TIFF_G4 库
- Telink — TLSR8258 SDK 与 tc32 工具链

## 许可

[MIT](LICENSE)。`ATC_TLSR_Paper/Firmware/src/LICENSE`（上游 MIT）随文件保留，Telink SDK 及第三方库按其原始许可分发。

## 免责声明

本项目与 Hanshow（汉朔）无关，仅供个人学习与硬件研究。刷机会覆盖原厂固件（原厂数据无法通过本固件访问），操作前请自行备份；因刷机造成的任何损失自负。
