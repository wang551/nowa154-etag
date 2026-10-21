# Nowa154-ESL：汉朔 Nowa-154R-N 电子价签开源固件

> **English TL;DR** — Open firmware adaptation for the Hanshow **Nowa-154R-N** e-paper
> shelf label (1.54" 152×152 black/white/**red** + NFC + BLE, Telink TLSR8359).
> It is a fork of [atc1441/ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper)
> with a properly reverse-engineered 152×152 panel driver (gate window 144..295,
> source offset +8), two new BLE commands (image hold, red plane), a fix for an
> upstream BLE advertising bug, and a single-file web upload tool with dithering
> and red-channel extraction. Flash over UART with a web flasher — no EVK needed.

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

## 仓库结构

```
├── ATC_TLSR_Paper/        # 固件源码（上游快照 + 本 fork 改动，自带 tc32 工具链）
│   └── Firmware/src/      #   改动集中在 epd*.c/h、app.c、ble.c、epd_ble_service.c
├── web_tools/             # 网页传图工具（单 HTML，Chrome/Edge）
├── bin/                   # 可直接刷写的固件
├── tools/                 # 构建重试脚本、152×152 布局预览器（Python）
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

## 固件版本

| 文件 | md5 | 状态 |
|---|---|---|
| `bin/ATC_Paper_nowa154_final.bin` | `ef7456e09df34ead39cc581be6cfa968` | ✅ 稳定版（真机验证：屏贴合、传图像素级正确） |
| `bin/ATC_Paper_nowa154_red.bin` | `12943dd8b46d049dbaa6fb8513e50ec4` | 🔬 最新构建：红通道 + 温度符号修正 + 广播恢复修复。红通道尚未真机验证（判读方法见下） |

`ATC_TLSR_Paper/Firmware/ATC_Paper.bin` 是当前源码的最新构建，与 `_red` 相同。

**红通道验证判读**：发测试图案后——① 红三角出现在右上且红横条全宽贴边 = 红平面映射正确；② 红色错位/镜像 = 只需改网页端 `packRedBuffer()` 映射，免重刷；③ 红色完全不显 = OTP 波形不含红驱动，需另加三色 LUT（参考 stellar-L3N-etag 的 `LUT_bwr_296` 机制）。

## 编译

需要安装 Python。Windows 下进入 `ATC_TLSR_Paper/Firmware/` 运行 `makeit.exe`，Linux 下 `make`（tc32 工具链已随仓库自带）。

两个已知的坑：

1. **tc32-elf-gcc 4.5.1 随机 internal compiler error**（`decl_attributes`，单文件约 10–20% 概率）：增量重跑即可收敛，直接用 `tools/build_retry.sh`（循环重试直到成功）
2. **make 不跟踪头文件依赖**：改过 `*.h` 后必须先删 `out/` 和 `ATC_Paper.bin` 强制全量重编，否则产物与旧版 md5 完全相同

## 文档

- [docs/hardware_notes.md](docs/hardware_notes.md) — 硬件鉴定：型号解码、TLSR8359、FM11NC08I NFC 芯片、烧录触点与引脚全表
- [docs/screen_adaptation.md](docs/screen_adaptation.md) — 152×152 玻璃映射的逆向推导（四轮刷机实验定出栅极/源极偏移）与像素打包公式
- [docs/ble_protocol.md](docs/ble_protocol.md) — BLE 服务/特征 UUID、命令表（含新增 `0x05`/`0x06`）、发送序列、Windows GATT 排坑

## 已知限制与路线

- [ ] 红通道真机验证（代码与网页工具已就绪）
- [ ] NFC（FM11NC08I）：I2C 探测 → NDEF 写入 → RF 场唤醒翻页。I2C 地址 0xAE、IRQ_N 低有效开漏
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
