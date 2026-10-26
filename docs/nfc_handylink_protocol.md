# Handy Link APK 静态逆向报告（2026-10-26）

> 样本：`Handy Link_3.0.1.apks`（com.hanshow.handylink，v3.0.1，Google Play 版，
> 存 `_local/apk/`；jadx 源码 `_local/apk/handylink_src/`，arm64 so 在 `_local/apk/hl_arm64/`）。
> 结论先行：**Handy Link 的 NFC 层是标准 NDEF 读写，没有任何自定义 ISO-DEP/APDU 协议可挖**。
> 厂商秘密不在 App 里——在价签侧（原厂固件如何让 FM11NC08I 服务 T4T）。

## 1. 应用架构

- Flutter 应用（Dart AOT 快照 `libapp.so` 11MB；Java 层只是壳 + 插件桥）。
- 企业级登录：MSAL（微软身份）+ TOTP（Google Authenticator，`/totp_binding`）——
  与用户实测"要登录、无法自助注册"一致。动态分析走不到 NFC 环节，但静态不受影响。
- 云端：`*.hanshowcloud.net`（如 `coles-prd.hanshowcloud.net`——Coles 澳洲生产环境）、
  `.hanshow.online`；资产 `discovery_packet` = hanshow.com IoT 平台 X.509 根证书（TLS 锁定）。
- 含 quickjs 热更运行时（`libfastdev_quickjs_runtime.so`）与加密配置 `manager.hse`。
- 无 BLE 专有协议痕迹（无 flutter_blue、无任何服务 UUID 字符串）——**数据通道不是手机↔标签 BLE**。
  文案与流程证实网络架构：LCD 型 ESL 走店内 Wi-Fi 连 AP；电子纸 ESL 走 2.4G 私有协议连 AP
  （ESL-Working 基站即"AP"）；手机只跟云/局域网 AP 交互。

## 2. NFC 层（核心结论）

- NFC 栈 = 开源插件 **flutter nfc_manager**（方法通道 `plugins.flutter.io/nfc_manager`，
  Java 侧 `z9/c.java`）：ReaderMode 轮询 + 全套 TagTechnology 透传（NfcA/IsoDep/Ndef/
  MifareClassic…的 `#transceive`/`#read`/`#write` 桥）。这是通用插件能力，**不代表业务用到**。
- libapp.so 全量字符串扫描（Dart AOT 的字符串常量是明文）：
  - **无** `D2760000850101`（NDEF AID）、无 `00A404` 类 APDU 十六进制、无 AID 表、
    无任何自定义帧模板 ⇒ 业务代码不构造 ISO-DEP 帧；
  - Dart 侧只有 NDEF 记录级处理：`nfcWellknown`（TNF well-known 解析）、`nfcExternal`、
    `nfcEvent`、`read nfc error message: `；
  - 流程文案（多语言 i18n）：`Select the ESL using NFC`、`Association/Disassociation EE|Produit par NFC`、
    `Configuration rapide par NFC`、`Gebruik NFC om de AP-configuratie te lezen`
    （**用 NFC 读 AP 配置——AP 设备里也有 NFC 标签**）、`Esta etiqueta NFC no es compatible`
    （"标签不支持"报错——按 NDEF 内容判型）、`Flush NFC`。
- 用途定性：**NFC = 身份卡读取（NDEF 里的条码/ID，如 `54-8F-95-8B`）+ 设备选择**；
  绑定/解绑/闪灯等操作经云端+AP 下发，不经 NFC。

## 3. 对本仓战役的意义

1. App 侧没有可重放的厂商帧 ⇒ 路径"②手机发帧重放"降级（无弹幕表可发；通用 NDEF 读取
   TagInfo/NFC Tools 已覆盖）。
2. 阶段 0 的原厂 TagInfo 实测（NDEF Text 记录=条码、CC `00 0F 20 00 12 00 18 04 06 E1 04 03 84 00 00`、
   读尾段 `read error [No response]`）与"App 只做 NDEF 读"完全自洽——**原厂价签的
   T4T NDEF 服务是唯一需要攻克的厂商秘密**，且其失败尾巴（尾部读无响应）像是
   极简应答器（MCU over FIFO，maotek 同款形态）或芯片自主服务的边界。
3. 判别"芯片自主服务 vs 原厂固件经 FIFO 应答"的决定性实验（免工具）：
   **抠电池后碰卡**——断电后场供电仍可读 = 芯片自主（EEPROM 直出）；读不出 = 原厂 MCU
   在应答（vendor init 序列是钥匙，SWS 读回 tag#3 原厂固件升为关键路径）。
   对照组：tag#1（v40）同样抠电池碰一次（排除我方固件轮询干扰芯片的可能性）。
