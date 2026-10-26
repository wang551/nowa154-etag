#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BLE OTA 刷机 + flash 读回（bleak 版，复刻 web_tools/nowa152_image_upload.html 的 OTA 管线）

用法:
  python tools/ble_ota_flash.py flash Firmware/ATC_Paper.bin   # 擦除->传输->校验->守卫->刷入
  python tools/ble_ota_flash.py read <addr> <len> [out.bin]    # 经 cmd4 读任意 flash 地址
  python tools/ble_ota_flash.py readback                        # 读回 NFC 探测结果区 0x40000
  python tools/ble_ota_flash.py eeread <hexaddr> [len]          # NFC 芯片 EEPROM 在线读（v20）
  python tools/ble_ota_flash.py eewrite <hexaddr> <hexbytes>    # NFC 芯片 EEPROM 在线写（≤12B）
  python tools/ble_ota_flash.py eereset                         # NFC 芯片软复位（重读 EEPROM 配置）

命令流与固件 ota.c 一一对应（详见 web_tools OTA 注释与 docs/ble_protocol.md）：
  cmd1 擦 4KB 扇区 / cmd3 填 RAM 缓冲 / cmd2 按 256B 落暂存区
  cmd6 设备端字节和（notify 首字节 0x07）/ cmd5 RAM 回读（守卫用）/ cmd7 刷入并重启
关键坑：cmd6 之后必须守卫复位（03+20x00、05+addr0）才能通过固件 cmd7 的
  out_buffer[5..6]==crc_out(0) 检查，否则 07 被静默拒绝。
"""

import asyncio
import sys
import time

from bleak import BleakScanner, BleakClient

OTA_SERVICE = "0000221f-0000-1000-8000-00805f9b34fb"
OTA_CHAR = "0000331f-0000-1000-8000-00805f9b34fb"
OTA_BANK = 0x20000
OTA_BANK_MAX = 0x1FF00
NAME_PREFIX = "ESL"
RESULT_ADDR = 0x40000
RESULT_LEN = 0x6C0  # v18: 0x650 起 = EEPROM 写解锁矩阵


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def addr_frame(cmd, addr):
    return bytes([cmd, (addr >> 24) & 255, (addr >> 16) & 255, (addr >> 8) & 255, addr & 255])


def local_sum(data):
    """bin 按 0xFF 补齐到 0x20000 后逐字节求和，与固件 cmd6 同式。"""
    s = sum(data) & 0xFFFF
    return (s + (OTA_BANK - len(data)) * 0xFF) & 0xFFFF


class OtaSession:
    def __init__(self, client, chunk):
        self.client = client
        self.chunk = chunk
        self.queue: asyncio.Queue = asyncio.Queue()

    async def start(self):
        await self.client.start_notify(OTA_CHAR, self._on_notify)
        await asyncio.sleep(0.3)  # CCC 生效窗口（首帧 notify 偶发被吞）

    def _on_notify(self, _char, data):
        self.queue.put_nowait(bytes(data))

    async def write(self, frame):
        await self.client.write_gatt_char(OTA_CHAR, frame, response=True)

    async def wait_notify(self, timeout_ms):
        try:
            return await asyncio.wait_for(self.queue.get(), timeout_ms / 1000)
        except asyncio.TimeoutError:
            return None

    async def drain(self):
        while not self.queue.empty():
            self.queue.get_nowait()

    async def erase_bank(self):
        log("OTA：擦除暂存区（32 x 4KB）…")
        for i in range(32):
            await self.write(addr_frame(0x01, OTA_BANK + i * 0x1000))

    async def upload(self, data):
        banks = (len(data) + 0xFF) // 0x100
        t0 = time.time()
        log(f"OTA：传输 {len(data)} 字节（{banks} x 256B，分块 {self.chunk}B）…")
        for b in range(banks):
            off, end = b * 0x100, min((b + 1) * 0x100, len(data))
            p = off
            while p < end:
                take = min(self.chunk, end - p)
                await self.write(bytes([0x03]) + data[p:p + take])
                p += take
            await self.write(addr_frame(0x02, OTA_BANK + off))
            await asyncio.sleep(0.05)
            if b % 32 == 0 or b == banks - 1:
                log(f"  {end}/{len(data)} 字节 … {time.time() - t0:.0f}s")

    async def verify_sum(self, expect):
        log("OTA：请求设备端校验和（cmd 06）…")
        await self.drain()
        await self.write(bytes([0x06]))
        nv = await self.wait_notify(5000)
        if not nv or nv[0] != 0x07:
            raise RuntimeError("cmd 06 校验应答超时/格式错误")
        got = (nv[1] << 8) | nv[2]
        if got != expect:
            raise RuntimeError(f"校验不一致：设备 0x{got:04x} != 本地 0x{expect:04x}（已拒绝刷入）")
        log(f"OTA：校验一致 0x{expect:04x}")

    async def guard_reset(self):
        """06 之后 crc_out=校验和、out_buffer[0..2] 被占用，固件 cmd7 的
        out_buffer[5..6]==crc_out 守卫不通过；发 03+20x00 清 crc_out，
        再 05+addr0 让 out_buffer 全零（notify 回读确认为全零）。"""
        log("OTA：守卫复位握手 …")
        await self.write(bytes([0x03]) + bytes(20))
        await self.write(addr_frame(0x05, 0))
        zv = await self.wait_notify(2000)
        if not zv or zv[0] != 0x00 or any(zv):
            raise RuntimeError("守卫复位握手失败（回读非全零）")

    async def final_flash(self, data_sum):
        log("OTA：发出刷入命令（07 C001CEED），设备将擦写自身并重启 …")
        try:
            await self.write(bytes([0x07, 0xC0, 0x01, 0xCE, 0xED, (data_sum >> 8) & 255, data_sum & 255]))
        except Exception as e:
            # 预期行为：设备收到 07 即关中断擦写自身，ATT 回执不会回来，
            # WinRT 会报"操作已被用户取消"——成功与否以断连/重启为准
            log(f"  写回执异常（{e}）——属预期，设备应在擦写中")


async def find_device(scan_s=15.0):
    log(f"扫描 {NAME_PREFIX}* 设备 {scan_s:.0f}s …")
    deadline = time.time() + scan_s
    while time.time() < deadline:
        for d in await BleakScanner.discover(timeout=5.0):
            if d.name and d.name.startswith(NAME_PREFIX):
                log(f"找到 {d.name} ({d.address})")
                return d
    raise RuntimeError("未发现设备（确认标签已上电、蓝牙可用）")


async def connect_retry(attempts=6):
    for i in range(attempts):
        try:
            d = await find_device(scan_s=15.0 if i == 0 else 10.0)
            client = BleakClient(d)
            await client.connect()
            log(f"已连接（MTU {client.mtu_size}）")
            return client
        except Exception as e:
            log(f"连接失败（{i + 1}/{attempts}）：{e}")
            await asyncio.sleep(2)
    raise RuntimeError("多次连接失败")


async def do_flash(bin_path):
    data = open(bin_path, "rb").read()
    if len(data) < 12 or data[8:12] != b"KNLT":
        raise RuntimeError("缺少 Telink 固件魔数 KNLT")
    if len(data) > OTA_BANK_MAX:
        raise RuntimeError(f"固件 {len(data)} 字节超出 OTA 上限")
    data_sum = local_sum(data)
    log(f"固件 {bin_path}：{len(data)} 字节，校验和 0x{data_sum:04x}")

    client = await connect_retry()
    try:
        chunk = 240 if client.mtu_size >= 244 else 19
        for attempt in (1, 2):
            sess = OtaSession(client, chunk)
            await sess.start()
            try:
                await sess.erase_bank()
                await sess.upload(data)
                await sess.verify_sum(data_sum)
                await sess.guard_reset()
                break
            except Exception as e:
                if attempt == 1 and chunk == 240:
                    log(f"大包失败（{e}）→ 疑似 MTU，降级 19B 整轮重试")
                    chunk = 19
                else:
                    raise
        await sess.final_flash(data_sum)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    log("等待设备刷写并重启（约 10s）…")
    await asyncio.sleep(10)
    log("刷入流程结束。用 readback 验证新固件探测结果。")


async def do_readback(out_path="nfc_probe_dump.bin"):
    client = await connect_retry()
    sess = OtaSession(client, 240)
    await sess.start()
    try:
        # 首帧写入会触发 otaWritePre 提速连接（interval -> 7.5ms）
        # cmd4 每次固定回 20B：向上取整到 20 的倍数读，末尾裁剪
        total = ((RESULT_LEN + 19) // 20) * 20
        result = bytearray()
        addr = RESULT_ADDR
        while addr < RESULT_ADDR + total:
            ok = False
            for retry in range(3):
                await sess.drain()
                await sess.write(addr_frame(0x04, addr))
                nv = await sess.wait_notify(2000)
                if nv and len(nv) == 20:
                    result += nv
                    ok = True
                    break
                log(f"  读 {addr:#x} 超时，重试 {retry + 1}/3")
            if not ok:
                raise RuntimeError(f"读 {addr:#x} 连续失败")
            addr += 20
        result = bytes(result[:RESULT_LEN])
    finally:
        await client.disconnect()

    open(out_path, "wb").write(result)
    log(f"已保存 {len(result)} 字节到 {out_path}")
    hdr = result[:8]
    print(f"魔数: {hdr[:4]!r}  版本: {hdr[4]}  状态: {hdr[5]}  I2C分频: {hdr[6]}  IRQ位样: {hdr[7]:02x}")
    if hdr[:4] != b"NFCP":
        print("!! 魔数不符 —— 新探测固件可能未运行（0x40000 是残留数据或旧固件）")
        return
    bm_low = result[0x08:0x18]
    bm_high = result[0x18:0x28]
    def acks(bm):
        return [a for a in range(0x7F) if bm[a >> 3] & (0x80 >> (a & 7))]
    print(f"ACK(CSN低): {[f'{a:#04x}' for a in acks(bm_low)]}")
    print(f"ACK(CSN高): {[f'{a:#04x}' for a in acks(bm_high)]}")
    dump2 = result[0x028:0x428]
    dump1 = result[0x428:0x528]
    hi2 = result[0x528:0x628]
    def hexrow(b, n=16):
        return " ".join(f"{x:02x}" for x in b[:n])
    print("EEPROM@0x000 (2字节寻址) 前 64B：")
    for i in range(0, 64, 16):
        print(f"  {i:04x}: {hexrow(dump2[i:])}")
    print(f"EEPROM 0x390-0x3FF（配置区，2字节寻址）：")
    for i in range(0x390, 0x400, 16):
        print(f"  {i:04x}: {hexrow(dump2[i:])}")
    print("1字节寻址对照 @0x000 前 64B：")
    for i in range(0, 64, 16):
        print(f"  {i:04x}: {hexrow(dump1[i:])}")
    print("0xFF00 窗（2字节寻址）前 64B：")
    for i in range(0, 64, 16):
        print(f"  {i:04x}: {hexrow(hi2[i:])}")
    same = all(dump2[i] == dump1[i] for i in range(0x100))
    print(f"前 256B 两种寻址结果{'完全一致' if same else '不一致（可判别寻址格式）'}")

    # v18 EEPROM 写解锁策略矩阵（'NFM1' @0x650）
    mtx = result[0x650:0x6C0]
    if mtx[:4] == b"NFM1":
        print(f"EEPROM 写矩阵: 成功策略={mtx[4] or '无'}  NDEF 改写={'成功' if mtx[5] else '未成/未做'}")
        for i in range(8):
            e = mtx[0x10 + i * 10: 0x10 + i * 10 + 10]
            if e[0] == 0:
                break
            pat = bytes((0x5A ^ (e[0] << 4) ^ k) & 255 for k in range(8))
            mark = "  <-- 写入成功!" if bytes(e[2:10]) == pat else ""
            print(f"  S{e[0]}: AuxIrq={e[1]:02x}  读回={e[2:10].hex(' ')}{mark}")
        print(f"  NDEF 块写后读回: {mtx[6:14].hex(' ')}")


async def do_read(addr, length, out_path=None):
    client = await connect_retry()
    sess = OtaSession(client, 240)
    await sess.start()
    try:
        result = bytearray()
        a = addr
        while a < addr + length:
            got = None
            for retry in range(4):
                await sess.drain()
                await sess.write(addr_frame(0x04, a))
                nv = await sess.wait_notify(3000)
                if nv and len(nv) == 20:
                    got = nv
                    break
                log(f"  读 {a:#x} 超时，重试 {retry + 1}/4")
            if not got:
                raise RuntimeError(f"读 {a:#x} 连续失败")
            result += got
            a += 20
    finally:
        await client.disconnect()
    if out_path:
        open(out_path, "wb").write(result)
        log(f"已保存 {len(result)} 字节到 {out_path}")
    else:
        print(result.hex())


async def do_ee(op, args):
    """NFC EEPROM 在线控制台（v20 固件，ota case 8/9/A）：
    eeread <hexaddr> [len] / eewrite <hexaddr> <hexbytes> / eereset
    """
    client = await connect_retry()
    sess = OtaSession(client, 240)
    await sess.start()
    try:
        if op == "eereset":
            await sess.drain()
            await sess.write(bytes([0x0A]))
            nv = await sess.wait_notify(4000)
            if not nv or nv[0] != 0x8A:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            names = ["FFE0", "FFE1", "FFE2", "Status", "TxEn", "MainIrqM"]
            print(f"ret={nv[1]}  " + "  ".join(f"{n}={nv[2 + i]:02x}" for i, n in enumerate(names)))
            return
        if op == "nfcdiag":
            await sess.drain()
            await sess.write(bytes([0x0B]))
            nv = await sess.wait_notify(4000)
            if not nv or nv[0] != 0x8B:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            names = ["FifoCnt映射", "FifoCnt命令", "Status映射", "Status命令", "MainIrq", "Rats", "TxEn", "AuxIrq", "NfcCfg"]
            print(f"ret={nv[1]}  " + "  ".join(f"{n}={nv[2 + i]:02x}" for i, n in enumerate(names)))
            return
        if op == "regwrite":
            if len(args) < 2:
                raise RuntimeError("regwrite <hexaddr> <hexval>")
            addr = int(args[0], 16)
            val = int(args[1], 16)
            await sess.drain()
            await sess.write(bytes([0x0C, addr >> 8, addr & 0xFF, val]))
            nv = await sess.wait_notify(4000)
            if not nv or nv[0] != 0x8C:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            print(f"ret={nv[1]}  {addr:04x}←{val:02x} 读回={nv[2]:02x}")
            return
        if op == "eesnap":
            # v34: eesnap <hexaddr> <len> —— EEPROM 快照到 flash 0x45000，随后用 read 取回
            if len(args) < 2:
                raise RuntimeError("eesnap <hexaddr> <len>")
            a = int(args[0], 16); ln = int(args[1], 0)
            await sess.drain()
            await sess.write(bytes([0x10, a >> 8, a & 0xFF, (ln >> 8) & 0xFF, ln & 0xFF]))
            nv = await sess.wait_notify(15000)
            if not nv or nv[0] != 0x90:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            print(f"ret={nv[1]}  快照 {a:#05x}+{ln} → flash 0x45000（用 read 45000 {ln} 取回）")
            return
        if op in ("dwrite", "magwrite"):
            # v34: dwrite/magwrite <hexaddr> <hexbytes> —— 直写（非 RMW）/ {CE,55} magic+直写
            if len(args) < 2:
                raise RuntimeError(f"{op} <hexaddr> <hexbytes>")
            a = int(args[0], 16); dat = bytes.fromhex(args[1])
            cmd = 0x11 if op == "dwrite" else 0x12
            mark = 0x91 if op == "dwrite" else 0x92
            await sess.drain()
            await sess.write(bytes([cmd, a >> 8, a & 0xFF]) + dat)
            nv = await sess.wait_notify(10000)
            if not nv or nv[0] != mark:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            aux = nv[2]; rb = nv[3:3 + len(dat)]
            print(f"ret={nv[1]}  {a:#05x}←{dat.hex(' ')}  AuxIrq={aux:02x} 读回={rb.hex(' ')}")
            if rb == dat:
                print("  <-- 写入成功！" + ("（bit7 写完成）" if aux & 0x80 else ""))
            elif aux & 0x40:
                print("  <-- 写被拒（AuxIrq bit6）")
            else:
                print("  <-- 读回不一致")
            return
        if op == "unstick":
            # v33d: 手动解卡（9 时钟 + 断电 20ms + 验证）
            await sess.drain()
            await sess.write(bytes([0x0F]))
            nv = await sess.wait_notify(8000)
            if not nv or nv[0] != 0x8F:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            n = ["Flush", "Cnt", "Status", "TxEn", "Baud", "Rats"]
            print(f"ret={nv[1]}  " + "  ".join(f"{n[i]}={nv[2+i]:02x}" for i in range(6)))
            print("  <-- 芯片恢复健康" if nv[1] == 0 else "  <-- 仍楔死")
            return
        if op == "cfgwrite":
            # v33: cfgwrite <c0> <c1> <c2> [mode]  —— 写 0x390 实际配置字（CHK 自动计算）
            if len(args) < 3:
                raise RuntimeError("cfgwrite <c0hex> <c1hex> <c2hex> [mode 0=裸写+软复位 1=静默包裹]")
            c0, c1, c2 = (int(a, 16) for a in args[:3])
            mode = int(args[3], 0) if len(args) > 3 else 0
            chk = (~(c0 ^ c1 ^ c2)) & 0xFF
            print(f"配置字 0x390 ← {c0:02x} {c1:02x} {c2:02x} {chk:02x}（CHK=~^） mode={mode}")
            await sess.drain()
            await sess.write(bytes([0x0D, c0, c1, c2, mode]))
            nv = await sess.wait_notify(8000)
            if not nv or nv[0] != 0x8D:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            ret = nv[1]
            w = nv[2:6]; ffe = nv[6:9]
            print(f"ret={ret}  0x390读回={w.hex(' ')}  FFE0/1/2={ffe[0]:02x}/{ffe[1]:02x}/{ffe[2]:02x}"
                  f"  AuxIrq={nv[9]:02x}  MIM={nv[10]:02x}")
            if ret == 0 and w == bytes([c0, c1, c2, chk]):
                print("  <-- 0x390 写入成功")
            arb = (ffe[2] >> 4) & 3
            print(f"  仲裁位={arb}({'00无优先级' if arb==0 else '01FCFS' if arb==1 else '10接触优先' if arb==2 else '11非接优先'}) "
                  f"ct_sleep={ffe[2] & 0xF:x}({'常上电' if (ffe[2]&0xF)>=8 else '会休眠'})")
            return
        if op == "masktest":
            # v33: 同一挂载会话内 0xFFFA←0xE5，2ms/10ms 两次回读（不 CSN 翻转）
            await sess.drain()
            await sess.write(bytes([0x0E]))
            nv = await sess.wait_notify(8000)
            if not nv or nv[0] != 0x8E:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            n = ["写前", "即读", "+2ms", "+10ms"]
            print(f"ret={nv[1]}  " + "  ".join(f"{n[i]}={nv[2+i]:02x}" for i in range(4)) + f"  AuxIrq={nv[6]:02x}")
            if nv[3] == 0xE5 and nv[5] == 0xE5:
                print("  <-- 掩码写入成功且保持！ct_sleep 假设不成立（或配置已改常上电）")
            elif nv[3] == 0xE5:
                print("  <-- 写入成功但 10ms 内丢失（掉电回默认？）")
            else:
                print("  <-- 写入即失败（写通道被拒/楔死）")
            return
        if op == "fifotest":
            # v39: fifotest <mode> —— TX FIFO 落点自环（写3B图案不发TxEn，看计数/弹出）
            mode = int(args[0], 0) if args else 0
            await sess.drain()
            await sess.write(bytes([0x13, mode]))
            nv = await sess.wait_notify(6000)
            if not nv or nv[0] != 0x93:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            rb = nv[3:14]
            print(f"ret={nv[1]} mode={nv[2]}  写前cnt={rb[0]:02x} 写后cnt={rb[1]:02x} "
                  f"弹出={rb[2:8].hex(' ')} flush后cnt={rb[8]:02x} AuxIrq={rb[9]:02x} 命令cnt={rb[10]:02x}")
            grew = ((rb[1] - rb[0]) & 0x3F) == 3
            hit = rb[2:5] == bytes.fromhex("5a a5 3c")
            if grew and hit:
                print("  <-- 写入进入 FIFO 且可弹出！TX 路径接触侧成立")
            elif grew:
                print("  <-- 计数+3 但弹出非图案（读语义问题）")
            else:
                print("  <-- 计数未涨：写入未进 FIFO（TX 内容从未上空，重试级联假设成立）")
            return
        if op == "txdis":
            # v40: txdis <0|1> —— TX 全链路禁用开关（RAM 态，重启回 0=照常发）
            en = int(args[0], 0) if args else 0
            await sess.drain()
            await sess.write(bytes([0x15, en & 1]))
            nv = await sess.wait_notify(4000)
            if not nv or nv[0] != 0x95:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            print(f"ret={nv[1]}  TX={'已禁用（盲态机/BSM 照跑，仅不发）' if nv[2] else '照常发'}")
            return
        if op == "mutemode":
            # v41: mutemode <0|1> —— 静默模式：CSN 常低 + 零 I²C 流量（判芯片自应答）。
            # 置 1 后立即碰卡测 TagInfo；结束记得 mutemode 0（接触侧常上电耗电）。
            en = int(args[0], 0) if args else 0
            await sess.drain()
            await sess.write(bytes([0x16, en & 1]))
            nv = await sess.wait_notify(4000)
            if not nv or nv[0] != 0x96:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            print(f"ret={nv[1]}  静默模式={'开启（CSN 常低+总线零流量，去碰卡吧）' if nv[2] else '关闭（恢复轮询/CSN 空闲断电）'}")
            return
        if op == "vendorseq":
            # v40: vendorseq <hexscript> —— 厂商接触侧配方回放。
            # 脚本 = 8B 定长记录串（≤30 条）: [type][addr_hi][addr_lo][len][p1..p4]
            #   0=EEPROM 写(+10ms)  1=读≤4B(结果进 notify)  2=延时 p1 ms  3=原始帧写
            #   4=CSN 断电 p1 ms     5=寄存器窗快写(+2ms)     6=FIFO flush
            # 例（读 0x3B5 → 写 0x3B0←A0 → 读回，各记录补零到 8B）：
            #   vendorseq "0103b501000000000003b001a00000000103b00100000000"
            if not args:
                raise RuntimeError("vendorseq <hexscript>")
            dat = bytes.fromhex(args[0].replace(" ", ""))
            if len(dat) == 0 or len(dat) % 8 or len(dat) > 240:
                raise RuntimeError(f"脚本 {len(dat)}B 非法：须 8B 整数倍且 ≤240B")
            # 脚本经 case 3 帧进 RAM 缓冲（19B 小分块，不依赖 MTU），再 0x14 触发执行
            for p in range(0, len(dat), 19):
                await sess.write(bytes([0x03]) + dat[p:p + 19])
            await sess.drain()
            await sess.write(bytes([0x14, len(dat)]))
            nv = await sess.wait_notify(20000)
            if not nv or nv[0] != 0x94:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            n_op, dead, reads = nv[2], nv[3], nv[4:20]
            print(f"ret={nv[1]}  执行 {n_op} 条  收尾健康={'ok' if dead == 0 else '卡死!'}  读回={reads.hex(' ')}")
            if nv[1] == 3:
                print("  <-- 执行中总线卡死，先 unstick 再试")
            return
        addr = int(args[0], 16)
        if op == "eeread":
            length = int(args[1], 0) if len(args) > 1 else 16
            length = min(length, 16)
            await sess.drain()
            await sess.write(bytes([0x09, addr >> 8, addr & 0xFF, length]))
            nv = await sess.wait_notify(4000)
            if not nv or nv[0] != 0x89:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            print(f"ret={nv[1]}  {addr:03x}: {nv[2:2 + length].hex(' ')}")
        elif op == "eewrite":
            data = bytes.fromhex(args[1])
            if len(data) > 12:
                raise RuntimeError("单次最多 12 字节")
            await sess.drain()
            await sess.write(bytes([0x08, addr >> 8, addr & 0xFF]) + data)
            nv = await sess.wait_notify(8000)
            if not nv or nv[0] != 0x88:
                raise RuntimeError(f"应答异常: {nv.hex() if nv else None}")
            print(f"ret={nv[1]}  写后读回 {addr:03x}: {nv[2:2 + len(data)].hex(' ')}")
            if nv[2:2 + len(data)] == data:
                print("  <-- 写入成功")
            else:
                print("  <-- 读回不一致")
        else:
            raise RuntimeError(f"未知子命令 {op}")
    finally:
        await client.disconnect()


async def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "flash":
        await do_flash(sys.argv[2])
    elif cmd == "readback":
        await do_readback(*(sys.argv[2:3] or ["nfc_probe_dump.bin"]))
    elif cmd == "read":
        await do_read(int(sys.argv[2], 16), int(sys.argv[3], 0), sys.argv[4] if len(sys.argv) > 4 else None)
    elif cmd in ("eeread", "eewrite", "eereset", "nfcdiag", "regwrite", "cfgwrite", "masktest", "unstick", "eesnap", "dwrite", "magwrite", "fifotest", "txdis", "mutemode", "vendorseq"):
        await do_ee(cmd, sys.argv[2:])
    else:
        print(__doc__)


if __name__ == "__main__":
    asyncio.run(main())
