# AGENTS.md — 本仓库工作须知

汉朔 Nowa-154R-N 电子价签（1.54" 152×152 黑白红三色墨水屏，Telink TLSR8359，NFC 型）的开源固件 fork，基线为 atc1441/ATC_TLSR_Paper（上游快照 = 提交 `61045a9`，fork 改动 = `c80a8c5`，对比这两笔即可看全部改动）。**专机专用**：屏型号被强制为 BWR154，多型号需求请用上游。仓库文档（README、docs/）均为中文，新增文档保持中文。

## 目录

- `ATC_TLSR_Paper/` — 固件源码 + 自带 tc32 工具链（`tc32_windows/`、`tc32_linux/`，勿动）。fork 改动集中在 `Firmware/src/`：`epd.c`、`epd_bwr_154.c/h`、`app.c/h`、`ble.c`、`epd_ble_service.c`、各 `epd_*.c/h`（read_temp 返回 int8_t）、`obd.inl`
- `web_tools/nowa152_image_upload.html` — 单文件零依赖网页传图工具（Chrome/Edge Web BLE），须保持单文件形态
- `bin/` — 可刷写固件发布物
- `tools/` — `build_retry.sh`（构建重试）、`render.py`（152×152 时钟布局离线预览，需 Pillow，直接解析固件源码里的字体数据，无需编译）
- `docs/` — `hardware_notes.md`（硬件鉴定/烧录触点）、`screen_adaptation.md`（屏映射逆向推导）、`ble_protocol.md`（BLE 协议 + Windows GATT 排坑）
- `_local/` — **私有资料，已 gitignore，严禁提交或外传**（厂商版权 PDF、交接文档、参考仓库 stellar-L3N-etag）

## 构建

Windows：`cd ATC_TLSR_Paper/Firmware && ./makeit.exe`（推荐直接跑仓库根的 `tools/build_retry.sh`）。Linux：Firmware/ 下 `make`。

必知坑：**make 不跟踪头文件依赖**，改过任何 `*.h` 后必须先删 `Firmware/out/` 和 `Firmware/ATC_Paper.bin` 再构建，否则产物与旧版 md5 完全相同（等于白编）。

产物 `Firmware/ATC_Paper.bin` 已 gitignore；对外发布固件放 `bin/`（命名 `ATC_Paper_nowa154_*.bin`），并在 README 版本表登记 md5。

## 架构要点（改前必读）

- 屏几何：玻璃 152×152，画布 152×160，栅极窗口 Y=144..295，源极偏移 δ=8。改任何显示相关代码前先读 `docs/screen_adaptation.md`。
- `epd.c` 的 `EPD_detect_model()` 末尾**强制 `epd_model = 3`**（BWR154）是有意为之（探测序列对 1.54"/2.13" 失效），不要当 bug 修。
- BLE 命令 `0x00`–`0x04` 与上游兼容；`0x05`（图片锁定）/`0x06`（红平面）为本 fork 新增，旧固件走 default 安全忽略。改 BLE 前先读 `docs/ble_protocol.md`；命令解析在 `epd_ble_service.c`。
- `ble.c` 的 `ble_disconnect_callback()` 里补调 `app_switch_to_indirect_adv()` 是修复上游"断连后永不恢复广播"的 bug（否则断连一次标签就从所有扫描消失），勿删。
- 三色屏红平面链路：固件 `EPD_Display()` 的红图参数 ↔ 网页端 `packRedBuffer()`。红色错位/镜像只需改网页端映射，免重刷。
- `Firmware/make/tl_firmware_tools.py` 的 `combine` 子命令被本 fork **有意删除**（发布安全工具拒绝其 argv 写任意路径的模式），文件内留有 NOTE；确需恢复按注释从上游取回，勿直接"还原"。

## 约定

- 行尾：`.gitattributes` 已统一文本 LF 入库；`*.sh` 强制 LF，`*.bat`/`*.cmd` 强制 CRLF。不要提交混入 CRLF 的 `.c/.h` 改动。
- 不入库：构建产物（`Firmware/out/`、`Firmware/ATC_Paper.bin`）、`.mimosa/`、`.zcode/`、`_local/`、`tools/build.log`、`__pycache__/`。
- 网页工具必须零依赖单文件（`file://` 直开，或 `python -m http.server` 托管后走 localhost）。
- Windows BLE 排坑（网页工具已按此实现，勿改回）：`getPrimaryServices()` 全量枚举在 Windows 栈返回空数组，必须 `getPrimaryService(uuid)` 按 UUID 单查。
