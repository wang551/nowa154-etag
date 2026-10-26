#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TLSR8359 SWS 直驱读取器（USB-UART + TX/RX 双接 SWS 线，只读工具）

背景（docs/nfc_v40_playbook.md 阶段 4 / tag#3 原厂固件尸检）：
atc1441 的网页烧录器（ATC_TLSR_Paper_UART_Flasher）是 TX 单向只写，无法 dump。
本工具按三份权威实现拼出完整双向 swire：
  1. 线编码（1 bit = 1 个 UART 字节：0x80=1、0xFE=0；帧=0x5A+addr24+RW_ID+data+END）
     —— atc1441 web flasher JS（写路径，在本型号真机验证过）
  2. 协议规格（bit0=1 单位低+4 高、bit1=4 低+1 高；读=主机逐字节发 1 单位低脉冲）
     —— pvvx/TLSRPGM 仓 TelinkSWire/README.md（Telink 官方协议文本）
  3. flash 桥读时序（0x0d=CSN、0x0c=SPI 桥、auto-read=0x0d←0x0A）
     —— pvvx/TLSRPGM UART2SWire 固件 Source/main.c read_flash()

速率方案（关键）：swire 从机按 0xb2 分频采样/发送。
  - 写路径用 DIV_W=55（atc 实证配对：UART 单元 0.80 字节，从机按脉冲宽度解码，宽裕）
  - 读路径切 DIV_R=160（从机位宽 50µs≈2.3 个 UART 字节，UART RX 逐位重组波形可解码）
  - 切回 55 时，0xb2 写帧必须用"慢速编码"（多字节拼慢波形）发，否则被 160 档误采样

接线（与网页烧录器的单线写不同，必须两线！）：
  价签 SWS 触点 ──┬── USB-UART 的 RXD（直连）
                 └── USB-UART 的 TXD（**串 ~1kΩ 电阻**，防从机回驱时电平顶牛）
  价签 GND ── USB-UART GND；价签 VCC ── 3V3
  可选：价签 NRST ── USB-UART 的 RTS（工具自动发复位脉冲；不接就靠 CPU-stop 连发抓窗口）

用法：
  python tools/sws_dump.py COM5 id              # 激活 + 读 reg 0x602/JEDEC/状态/头 32B
  python tools/sws_dump.py COM5 dump out.bin    # 全片 512KB dump（带进度/重试）
  python tools/sws_dump.py COM5 dump out.bin 0x0 0x1000   # 指定区间
  python tools/sws_dump.py COM5 regs            # swire 桥寄存器速览

安全声明：本工具不含任何 flash 写/擦代码（只有调试寄存器与 SPI 桥的易失写）。
"""

import os
import sys
import time
from pathlib import Path

import serial


def safe_write(path: str) -> str:
    """安全约束：只允许当前目录下的相对路径（同 tools/nfc_log_parse.py 约定）"""
    norm = path.replace("\\", "/")
    if os.path.isabs(norm) or ".." in norm.split("/"):
        raise SystemExit(f"非法输出路径 {path!r}：仅允许当前目录下的相对路径")
    return path


UART_BAUD = 460800
BIT_US = 1_000_000.0 / UART_BAUD   # 1 个 UART 位 = 2.174µs
BYTE_US = 10 * BIT_US              # 1 个 UART 字节 = 21.74µs

DIV_W = 55    # 写路径分频（atc web flasher 实证）
DIV_R = 160   # 读路径分频：单位 10µs，从机位宽 50µs
UNIT_W = 55 / 16_000_000 * 1e6      # 3.44µs
UNIT_R = 160 / 16_000_000 * 1e6     # 10.0µs

# ---------------------------------------------------------------- 快速编码层（从机在 DIV_W 下）

def cell_fast(bit: bool) -> int:
    return 0x80 if bit else 0xFE


def swire_byte_fast(byte: int, cmd_flag: bool) -> bytes:
    """1 个 swire 字节 = 10 个 UART 字节：[flag 位][8 数据位 MSB first][end]"""
    d = bytearray(10)
    d[0] = cell_fast(cmd_flag)
    m = 0x80
    for i in range(1, 9):
        d[i] = cell_fast((byte & m) != 0)
        m >>= 1
    d[9] = 0xFE
    return bytes(d)


def pkt_write_fast(addr: int, data: bytes) -> bytes:
    """swire 写帧（DIV_W 下发出）：0x5A(cmd) + addr24 + RW_ID=0 + data + END"""
    p = bytearray()
    p += swire_byte_fast(0x5A, True)
    for b in ((addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF):
        p += swire_byte_fast(b, False)
    p += swire_byte_fast(0x00, False)          # RW_ID：bit7=0 写
    for b in data:
        p += swire_byte_fast(b, False)
    p += bytes([0x80] * 9 + [0xFE])            # END = cmd 0xff
    return bytes(p)


def pkt_read_hdr_fast(addr: int) -> bytes:
    """swire 读帧头（DIV_W 下发出）：0x5A + addr24 + RW_ID=0x80，之后逐字节时钟"""
    p = bytearray()
    p += swire_byte_fast(0x5A, True)
    for b in ((addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF):
        p += swire_byte_fast(b, False)
    p += swire_byte_fast(0x80, False)          # RW_ID：bit7=1 读
    return bytes(p)


END_FAST = bytes([0x80] * 9 + [0xFE])

# ---------------------------------------------------------------- 慢速编码层（从机在 DIV_R 下）

# UART 字节 → 10 个位电平（start=0, b0..b7 LSB first, stop=1），供解码器拼精确波形
_BITS = []
for _v in range(256):
    _BITS.append(tuple([0] + [(_v >> _i) & 1 for _i in range(8)] + [1]))


def _wave(low_us: float, high_us: float) -> bytes:
    """用 UART 字节拼出 ≈(low, high) 的线波形（贪心，±4µs 精度足够协议判宽）"""
    out = bytearray()
    while low_us > 2.0:
        if low_us >= 9 * BIT_US:
            out.append(0x00)                    # 19.6 低（尾带 1 位高）
            low_us -= 9 * BIT_US
        elif low_us >= 6 * BIT_US:
            out.append(0xC0)                    # 13.0 低
            low_us -= 6 * BIT_US
        else:
            out.append(0x80)                    # 17.4 低（超配，解码侧按宽度判）
            low_us = 0.0
    while high_us > 2.0:
        if high_us >= 9 * BIT_US:
            out.append(0xFF)                    # 全高（首带 1 位低）
            high_us -= 9 * BIT_US
        else:
            out.append(0xFE)                    # 4.3 低 + 17.4 高
            low_us -= 2 * BIT_US
            high_us -= 8 * BIT_US
    if low_us > 0.5:
        out.append(0xFE)
    return bytes(out)


def swire_byte_slow(byte: int, cmd_flag: bool, unit: float, end: bool = True) -> bytes:
    """DIV_R 下的 1 个 swire 字节（cell = 5 单位；bit1=4u低+1u高）。
    end=True 输出协议要求的字节尾低（写给真实从机用）；end=False 用于本地测试向量
    ——真实总线上"尾低+下字节 flag 低"连续合并为下字节的第一个 cell。"""
    bits = [1 if cmd_flag else 0] + [(byte >> i) & 1 for i in range(7, -1, -1)]
    out = bytearray()
    for b in bits:
        if b:
            out += _wave(4 * unit, 1 * unit)
        else:
            out += _wave(1 * unit, 4 * unit)
    if end:
        out += _wave(1 * unit, 1 * unit)       # end：数单位低 + 释放
    return bytes(out)


def pkt_write_slow(addr: int, data: bytes, unit: float) -> bytes:
    p = bytearray()
    p += swire_byte_slow(0x5A, True, unit)
    for b in ((addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF):
        p += swire_byte_slow(b, False, unit)
    p += swire_byte_slow(0x00, False, unit)
    for b in data:
        p += swire_byte_slow(b, False, unit)
    p += swire_byte_slow(0xFF, True, unit)     # END
    return bytes(p)


# ---------------------------------------------------------------- 解码层

def decode_swire_bytes(rx: bytes, n_bytes: int, unit_us: float):
    """UART RX 字节流 → 精确位级波形 → 游程沿 → 按位宽分类重组 n_bytes 个 swire 字节。
    每 cell = (低, 高) 对：低 > 2.5 单位 → 1；否则 0。每字节 9 cell（flag+8）。
    双向毛刺滤波：UART 起始位会在填充中插入 ~2-4µs 假沿，须桥接合并。"""
    # 1) 位级拼波形 → 游程沿 [kind, µs]
    edges = []
    for v in rx:
        for level in _BITS[v]:
            dur = BIT_US
            if edges and edges[-1][0] == ('L' if level == 0 else 'H'):
                edges[-1][1] += dur
            else:
                edges.append(['L' if level == 0 else 'H', dur])
    # 2) 双向毛刺滤波（迭代至收敛）
    glitch = 0.6 * unit_us
    merged = [list(e) for e in edges]
    changed = True
    while changed:
        changed = False
        out = []
        i = 0
        while i < len(merged):
            if (i + 2 < len(merged) and merged[i + 1][1] < glitch
                    and merged[i][0] == merged[i + 2][0]):
                out.append([merged[i][0], merged[i][1] + merged[i + 2][1]])
                i += 3
                changed = True
            else:
                out.append(merged[i])
                i += 1
        merged = out
    # 2b) 残余亚单位短沿（如 0xFF 空闲字节的起始位）丢弃后重合并
    while any(e[1] < glitch for e in merged):
        kept = [e for e in merged if e[1] >= glitch]
        rle = []
        for e in kept:
            if rle and rle[-1][0] == e[0]:
                rle[-1][1] += e[1]
            else:
                rle.append(list(e))
        merged = rle
    # 3) cell 分类：跳过前导高，每 (L,H) 对 = 1 位
    cells = []
    i = 0
    if merged and merged[0][0] == 'H':
        i = 1
    thr = 2.5 * unit_us
    while i < len(merged):
        if merged[i][0] != 'L':
            i += 1
            continue
        cells.append(1 if merged[i][1] > thr else 0)
        i += 2
    if len(cells) < 9 * n_bytes:
        return None, f"cells {len(cells)}/{9 * n_bytes} 不足（rx={len(rx)}B）"
    out = bytearray()
    for k in range(n_bytes):
        nine = cells[k * 9:(k + 1) * 9]
        val = 0
        for bit in nine[1:9]:
            val = (val << 1) | bit
        out.append(val)
    return bytes(out), None


# ---------------------------------------------------------------- 传输层

CLOCK_BYTE = 0x00    # 读时钟脉冲：9 位低 ≈ 19.6µs ≈ 2.0 单位（< 2.5 阈值）


class SwirePort:
    def __init__(self, port: str):
        self.sp = serial.Serial(port, UART_BAUD, timeout=0.02)
        self.sp.setRTS(False)
        self.sp.setDTR(False)
        self.sp.reset_input_buffer()
        self.div = DIV_W                          # 开机默认即 55（与 atc 工具一致）

    def close(self):
        try:
            self.set_div(DIV_W)
        except Exception:
            pass
        self.sp.close()

    def raw(self, pkt: bytes):
        self.sp.reset_input_buffer()
        self.sp.write(pkt)
        self.sp.flush()

    def set_div(self, new: int):
        """写 0xb2。当前=DIV_W 用快帧；当前=DIV_R 用慢帧（否则被误采样）"""
        if new == self.div:
            return
        if self.div == DIV_W:
            self.raw(pkt_write_fast(0x00B2, bytes([new])))
        else:
            self.raw(pkt_write_slow(0x00B2, bytes([new]), UNIT_R))
        time.sleep(0.004)
        self.div = new

    def wr(self, addr: int, data: bytes):
        self.set_div(DIV_W)
        self.raw(pkt_write_fast(addr, data))

    def rd(self, addr: int, n: int, retries: int = 2):
        """寄存器读：帧头(55) → 切 160 → 逐字节时钟+解码 → 切回 55"""
        last_err = None
        for _try in range(retries):
            self.set_div(DIV_W)
            self.raw(pkt_read_hdr_fast(addr))
            self.set_div(DIV_R)
            out = bytearray()
            fail = None
            for _ in range(n):
                self.sp.reset_input_buffer()
                self.sp.write(bytes([CLOCK_BYTE]))
                self.sp.flush()
                t0 = time.time()
                buf = bytearray()
                while len(buf) < 30 and time.time() - t0 < 0.012:
                    buf += self.sp.read(30 - len(buf))
                if buf and buf[0] == CLOCK_BYTE:
                    buf = buf[1:]          # TX-RX 并联回显：跳过自己的时钟字节
                if not buf:
                    fail = "无响应"
                    break
                val, err = decode_swire_bytes(bytes(buf), 1, UNIT_R)
                if val is None:
                    fail = f"{err} raw={bytes(buf[:24]).hex(' ')}"
                    break
                out += val
            self.set_div(DIV_W)
            self.raw(END_FAST)                    # END 符号收尾
            if not fail:
                return bytes(out), None
            last_err = fail
            time.sleep(0.002)
        return None, last_err


# ---------------------------------------------------------------- 激活层

def activate(p: SwirePort, ms=15000, use_rts=True):
    """CPU-stop 连发抓唤醒窗（atc Activate 同款）；带 RTS 则先硬复位。
    原厂固件深睡窗口稀疏，默认连发 15s；还不中就配合拔插电池上电瞬抓。"""
    if use_rts:
        print("  RTS 复位脉冲 100ms（NRST 未接 RTS 则无效，靠连发抓窗口）…")
        p.sp.setRTS(True)
        time.sleep(0.1)
        p.sp.setRTS(False)
        time.sleep(0.01)
    print(f"  CPU-stop 连发 {ms}ms …")
    p.set_div(DIV_W)
    stop_pkt = pkt_write_fast(0x0602, b'\x05')
    t0 = time.time()
    n = 0
    while time.time() - t0 < ms / 1000.0:
        p.raw(stop_pkt)
        n += 1
    print(f"  发出 {n} 帧")
    val, err = p.rd(0x0602, 1)
    if err is None and val == b'\x05':
        print("  ✓ 激活成功：reg 0x602=0x05，且读路径打通")
        return True
    print(f"  ✗ 验证失败：{val.hex() if val else '-'} {err or ''}")
    return False


# ---------------------------------------------------------------- flash 桥（只读）

def _bridge_read(p: SwirePort, cmd_bytes: bytes, n: int):
    """csn 低 → 0x0c 写命令 → {0,0x0A} 启 auto-read（0x0c+0x0d 连写）→ swire 读 0x0c → csn 高"""
    p.wr(0x0D, b'\x00')                                    # CSN 低
    p.raw(pkt_write_fast(0x0C, cmd_bytes))                 # 命令+地址
    p.raw(pkt_write_fast(0x0C, b'\x00\x0A'))               # 0x00→0x0c 首读触发；0x0A→0x0d auto-read
    time.sleep(0.002)
    val, err = p.rd(0x0C, n)
    p.wr(0x0D, b'\x01')                                    # CSN 高
    return val, err


def flash_jedec(p: SwirePort):
    return _bridge_read(p, b'\x9F', 3)


def flash_status(p: SwirePort):
    return _bridge_read(p, b'\x05', 1)


def flash_read(p: SwirePort, addr: int, n: int):
    return _bridge_read(p, bytes([0x03, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF]), n)


# ---------------------------------------------------------------- 主流程

def cmd_id(p: SwirePort):
    if not activate(p):
        return 1
    print("\n[1] flash JEDEC ID：")
    v, e = flash_jedec(p)
    print("   ", v.hex(' ') if v else '-', e or '')
    print("[2] flash 状态寄存器：")
    v, e = flash_status(p)
    print("   ", v.hex(' ') if v else '-', e or '')
    print("[3] flash 头 32B：")
    v, e = flash_read(p, 0, 32)
    print("   ", v.hex(' ') if v else '-', e or '')
    return 0


def cmd_regs(p: SwirePort):
    if not activate(p):
        return 1
    for name, a in [("swire data 0xb0", 0xB0), ("swire ctrl 0xb1", 0xB1),
                    ("clk_div 0xb2", 0xB2), ("ctrl2 0xb3", 0xB3),
                    ("cpu ctrl 0x602", 0x602)]:
        v, e = p.rd(a, 1)
        print(f"  {name}: {v.hex() if v else '-'} {e or ''}")
    return 0


def cmd_dump(p: SwirePort, out_path: str, start: int, size: int):
    if not activate(p):
        return 1
    CHUNK = 16
    buf = bytearray()
    t0 = time.time()
    errs = 0
    addr = start
    end = start + size
    while addr < end:
        n = min(CHUNK, end - addr)
        v = None
        for _try in range(5):
            v, e = flash_read(p, addr, n)
            if v is not None and len(v) == n:
                break
            time.sleep(0.01)
        if v is None or len(v) != n:
            print(f"\n!! 0x{addr:06x} 连续失败：{e}")
            errs += 1
            if errs > 8:
                print("错误过多中止")
                return 1
            v = (v or b'') + b'\xff' * (n - len(v or b''))
        buf += v
        addr += n
        if (addr - start) % 2048 == 0:
            pct = (addr - start) / size * 100
            rate = (addr - start) / max(time.time() - t0, 0.001)
            eta = (end - addr) / max(rate, 1)
            print(f"\r  {pct:5.1f}%  0x{addr:06x}  {rate:.0f} B/s  ETA {eta/60:.1f}min",
                  end='', flush=True)
    print()
    Path(out_path).write_bytes(bytes(buf))  # out_path 已在 main() 经 safe_write 校验
    print(f"已写 {len(buf)} 字节 → {out_path}（{time.time()-t0:.0f}s）")
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    port, op = sys.argv[1], sys.argv[2]
    p = SwirePort(port)
    try:
        if op == 'id':
            return cmd_id(p)
        if op == 'regs':
            return cmd_regs(p)
        if op == 'dump':
            out = safe_write(sys.argv[3] if len(sys.argv) > 3 else 'tag_fw_512k.bin')
            start = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
            size = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x80000
            return cmd_dump(p, out, start, size)
        print(__doc__)
        return 1
    finally:
        p.close()


if __name__ == '__main__':
    sys.exit(main() or 0)
