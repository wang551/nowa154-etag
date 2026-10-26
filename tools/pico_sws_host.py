#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pico(RP2040) SWS 主机的 PC 侧驱动 —— 配套 tools/pico_sws_main.py（烧进 Pico 当 main.py）

背景：CH340 直驱的 swire 读路径已判死——TX 空闲推挽高电平把从机回驱顶死在总线上
（写通路正常，CPU-stop 命中过）。Pico GPIO 开漏+上拉 = 真正的双向单线，顶牛不存在。
所有 swire 位级时序在 Pico 端完成，本脚本只做串口编排（115200 行协议）。

用法（在仓库根目录运行；不带 -p 时自动选唯一的 Pico 串口 VID 2e8a，CH340 是 1a86）：
  python tools/pico_sws_host.py find                       # 列串口，标出 Pico
  python tools/pico_sws_host.py ping [-p COM7]             # Pico 端 main.py 存活（OK PONG）
  python tools/pico_sws_host.py act [-p COM7] [--ms 3000]  # 激活：CPU-stop 连发+校验 0x602=05
  python tools/pico_sws_host.py jedec [-p COM7]            # SPI flash JEDEC ID（冒烟）
  python tools/pico_sws_host.py frd [-p COM7] --addr 0 --n 32
  python tools/pico_sws_host.py header [-p COM7]           # flash 头 32B + TLNK 魔数检查
  python tools/pico_sws_host.py dump [-p COM7] --out _local/tag3_fw_512k.bin [--chunk 256] [--fresh]
                                      # 全片 512KB；断点续传（按已写文件长度），失败自动重激活
  python tools/pico_sws_host.py verify [-p COM7] --bin _local/tag3_fw_512k.bin [--windows 24]
                                      # 随机抽查复读比对（终证可整片复读）

dump 产物建议放 _local/（gitignore，严禁入库外传）。
"""

import argparse
import hashlib
import random
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("需要 pyserial：pip install pyserial")


PICO_VID = 0x2E8A
FLASH_SIZE = 0x80000
TLNK = b'TLNK'


def safe_write(path: str) -> str:
    """安全约束：只允许当前目录下的相对路径（同 tools/sws_dump.py 约定）"""
    norm = path.replace("\\", "/")
    if Path(norm).is_absolute() or ".." in norm.split("/"):
        raise SystemExit(f"非法输出路径 {path!r}：仅允许当前目录下的相对路径")
    return path


def safe_read(path: str) -> Path:
    """安全约束同上：只读当前工作目录下的文件，拒绝绝对路径与 .."""
    norm = path.replace("\\", "/")
    if Path(norm).is_absolute() or ".." in norm.split("/"):
        raise SystemExit(f"非法输入路径 {path!r}：仅允许当前目录下的相对路径")
    p = Path(norm)
    if not p.is_file():
        raise SystemExit(f"找不到文件 {path!r}")
    return p


def pick_port(want: str = None) -> str:
    ports = list(list_ports.comports())
    picos = [p for p in ports if (p.vid or 0) == PICO_VID]
    if want:
        return want
    if len(picos) == 1:
        return picos[0].device
    print("串口列表：")
    for p in ports:
        mark = "  <-- Pico" if (p.vid or 0) == PICO_VID else ""
        print(f"  {p.device}  vid={('%04x' % p.vid) if p.vid else '----'}:pid={('%04x' % p.pid) if p.pid else '----'}  {p.description}{mark}")
    if not picos:
        raise SystemExit("未找到 Pico（VID 2e8a）。先跑 find 检查；Pico 未刷 MicroPython 会枚举成其他 VID。")
    raise SystemExit("发现多个 VID 2e8a 设备，请用 -p COMx 指定。")


class Pico:
    """Pico 端 main.py 的行协议客户端：命令 → (data: bytes, msg: str|None)"""

    def __init__(self, port: str):
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.05)
        except serial.SerialException as e:
            raise SystemExit(f"打不开 {port}: {e}\n（被占用就关掉烧录器网页/串口助手；先跑 find 认口）")
        self.port = port
        if not self.alive():
            print("!! PING 无回应（main.py 没在跑 / 被别的程序占用）——先跑 ping 子命令确认")

    def close(self):
        self.ser.close()

    def alive(self) -> bool:
        """PING 探活（开机横幅是一次性的，端口后开就看不到了）"""
        for _ in range(3):
            self.ser.reset_input_buffer()
            self.ser.write(b"PING\n")
            self.ser.flush()
            deadline = time.time() + 1.5
            while time.time() < deadline:
                line = self.ser.readline().decode('utf-8', 'replace').strip()
                if line == 'OK PONG':
                    return True
        return False

    def cmd_line(self, line: str, timeout: float = 5.0):
        """返回整行响应（RAW 这类非 hex 数据用）"""
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            s = raw.decode('utf-8', 'replace').strip()
            if not s or s == line:
                continue
            return s
        return None

    def cmd(self, line: str, timeout: float = 5.0):
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            s = raw.decode('utf-8', 'replace').strip()
            if not s or s == line:
                continue                      # 回显/空行
            if s.startswith('ERR'):
                return b'', s
            if s.startswith('OK'):
                toks = s.split()
                for cand in (toks[2:3], toks[1:2]):   # "OK RD <hex>" / "OK 05"
                    if cand:
                        try:
                            return bytes.fromhex(cand[0]), None
                        except ValueError:
                            pass
                return b'', s                          # OK RST / OK PONG / OK SETU n 等
            # 其他行（横幅残段等）忽略
        return b'', '超时（Pico 无响应）'


# ---------------------------------------------------------------- 子命令

def wiring_ok(pk: Pico) -> bool:
    """电气自检 + 提示。注意：low 读成 1 不一定是接线错——
    PA7 同时是蓝色 LED 脚，原厂固件会当 GPIO 输出驱动，弱驱动(2mA)时会被顶高
    （固件已把驱动拉到 12mA）。这里只报警告，不阻断，除非 Pico 无响应。"""
    s = pk.cmd_line("PIN", timeout=3.0)
    if not s or not s.startswith('OK PIN'):
        print(f"!! PIN 自检无响应：{s}（Pico/main.py 未运行？）")
        return False
    kv = dict(p.split('=', 1) for p in s.split()[2:] if '=' in p)
    if kv.get('low') != '0':
        print(f"⚠ 自检：{s} —— 拉低时仍读高。可能是标签 PA7 正在驱动（LED 脚）或线接错；"
              "若接下来读不到应答，先断电重上电再试。")
    if kv.get('rel') != '1':
        print(f"⚠ 自检：{s} —— 释放后未回高（缺上拉或线断）")
    return True


def do_act(pk: Pico, ms: int, boot: bool = False) -> bool:
    if not wiring_ok(pk):
        return False
    for attempt in range(1, 6):
        if boot:
            data, msg = pk.cmd(f"BOOTACT {ms}", timeout=ms / 1000.0 + 8.0)
        else:
            data, msg = pk.cmd(f"ACT {ms}", timeout=ms / 1000.0 + 8.0)
        if data == b'\x05':
            print(f"✓ 激活成功（第 {attempt} 次）：reg 0x602 = 0x05，读写链路均通")
            return True
        print(f"  第 {attempt} 次未中（{data.hex() if data else '-'} {msg or ''}）")
    print("✗ 5 次未中。抓窗技巧：手机贴住标签维持 NFC 读卡（原厂固件场唤醒=MCU 醒着），"
          "同时重跑 act——手机上卡消失即 CPU-stop 命中。仍不中可换电池重上电瞬抓。")
    return False


def need_act(pk: Pico):
    """dump/verify 前先确认还活着：直接试读，失败才重新激活"""
    data, msg = pk.cmd("RD 602 1", timeout=3.0)
    if data == b'\x05':
        return True
    print("（0x602 读值异常，先重新激活）")
    return do_act(pk, 3000)

def do_frd(pk: Pico, addr: int, n: int, timeout: float = None):
    tmo = timeout if timeout is not None else 2.0 + n * 0.01
    return pk.cmd(f"FRD {addr:x} {n}", timeout=tmo)


def do_header(pk: Pico) -> int:
    if not need_act(pk):
        return 1
    jed, msg = pk.cmd("JED", timeout=4.0)
    print(f"JEDEC ID : {jed.hex(' ') if jed else '-'} {msg or ''}")
    data, msg = do_frd(pk, 0, 32)
    if not data:
        print("flash 头读取失败：", msg)
        return 1
    print("flash 32B:", data.hex(' '))
    if data[8:12] == TLNK:
        print("✓ 偏移 8 处 TLNK 魔数命中 —— 读通路完全打通，可以 dump")
        return 0
    print("!! 偏移 8 非 TLNK（原厂固件若非 Telink 标准头也会这样），dump 前先人工确认内容")
    return 0


def do_dump(pk: Pico, out_path: str, chunk: int, fresh: bool) -> int:
    out_path = safe_write(out_path)
    if not need_act(pk):
        return 1
    buf = bytearray(FLASH_SIZE)
    done = 0
    if not fresh:
        f = Path(out_path)
        if f.exists():
            prev = f.read_bytes()
            if 0 < len(prev) <= FLASH_SIZE:
                buf[:len(prev)] = prev
                done = (len(prev) // chunk) * chunk
                print(f"续传：已有 {len(prev)}B，从 0x{done:x} 继续（--fresh 可重头）")
    t0 = time.time()
    fails = 0
    ckpt_mark = done
    addr = done
    while addr < FLASH_SIZE:
        n = min(chunk, FLASH_SIZE - addr)
        data, msg = None, None
        for _try in range(5):
            data, msg = do_frd(pk, addr, n)
            if len(data) == n:
                break
            time.sleep(0.05)
        if len(data) != n:
            print(f"\n!! 0x{addr:x} 连续失败（{msg}），尝试重新激活…")
            if not do_act(pk, 1000):
                Path(out_path).write_bytes(bytes(buf[:done]))  # 断点保护
                print(f"已写断点到 {out_path}（{done}B）。修好后重跑同一命令即续传。")
                return 2
            data, msg = do_frd(pk, addr, n)
            if len(data) != n:
                fails += 1
                if fails > 8:
                    Path(out_path).write_bytes(bytes(buf[:done]))
                    print(f"错误过多中止；断点已保存（{done}B）→ {out_path}")
                    return 2
                data = data + b'\xff' * (n - len(data))   # 补占位继续（verify 阶段能抓出来）
        buf[addr:addr + n] = data
        addr += n
        done = addr
        if done - ckpt_mark >= 32768:                     # 每 32KB 落一次盘
            Path(out_path).write_bytes(bytes(buf[:done]))
            ckpt_mark = done
        pct = done / FLASH_SIZE * 100
        rate = done / max(time.time() - t0, 0.001)
        eta = (FLASH_SIZE - done) / max(rate, 1)
        print(f"\r  {pct:5.1f}%  0x{done:06x}  {rate:7.0f} B/s  ETA {eta / 60:4.1f}min",
              end='', flush=True)
    print()
    Path(out_path).write_bytes(bytes(buf))
    md5 = hashlib.md5(bytes(buf)).hexdigest()          # md5 仅为仓库双构建比对惯例
    sha = hashlib.sha256(bytes(buf)).hexdigest()
    print(f"完成：{FLASH_SIZE} 字节 → {out_path}  md5={md5}")
    print(f"  sha256={sha}  用时 {time.time() - t0:.0f}s")
    if buf[8:12] == TLNK:
        print("✓ 偏移 8 TLNK 魔数命中")
    else:
        print("!! 偏移 8 非 TLNK——抽查内容确认后再进入反汇编")
    print("下一步：python tools/pico_sws_host.py verify -p {port} --bin {out}".format(port=pk.port, out=out_path))
    return 0


def do_verify(pk: Pico, bin_path: str, windows: int) -> int:
    f = Path(bin_path)
    if not f.exists():
        raise SystemExit(f"找不到 {bin_path}")
    data = f.read_bytes()
    total = len(data)
    ws = min(256, total)
    rng = random.SystemRandom()
    offs = {0, 8, total - ws}
    while len(offs) < 2 + windows:
        offs.add(rng.randrange(0, total - ws, ws))
    if not need_act(pk):
        return 1
    bad = 0
    for off in sorted(offs):
        reread, msg = do_frd(pk, off, ws)
        if len(reread) != ws:
            print(f"0x{off:06x}: 读取失败 {msg}")
            bad += 1
        elif reread != data[off:off + ws]:
            print(f"0x{off:06x}: 不一致！")
            print("  dump:", data[off:off + 16].hex(' '))
            print("  复读:", reread[:16].hex(' '))
            bad += 1
    if bad == 0:
        print(f"✓ 抽查 {len(offs)} 窗全一致（{total}B）。如需终证可再整片 dump 到另一文件比对 md5")
        return 0
    print(f"✗ {bad}/{len(offs)} 窗不一致——建议 --fresh 重新 dump")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='op', required=True)
    sp.add_parser('find')
    for name in ('ping', 'act', 'jedec', 'frd', 'header', 'dump', 'verify', 'check'):
        q = sp.add_parser(name)
        q.add_argument('-p', '--port', default=None)
    sp.choices['act'].add_argument('--ms', type=int, default=3000)
    sp.choices['act'].add_argument('--boot', action='store_true',
                                   help='先硬复位（需 GP3→NRST）再连发，抓启动窗口')
    frd_q = sp.choices['frd']
    frd_q.add_argument('--addr', type=lambda x: int(x, 0), default=0)
    frd_q.add_argument('--n', type=int, default=32)
    dump_q = sp.choices['dump']
    dump_q.add_argument('--out', default='_local/tag3_fw_512k.bin')
    dump_q.add_argument('--chunk', type=int, default=256)
    dump_q.add_argument('--fresh', action='store_true')
    ver_q = sp.choices['verify']
    ver_q.add_argument('--bin', default='_local/tag3_fw_512k.bin')
    ver_q.add_argument('--windows', type=int, default=24)
    args = ap.parse_args()

    if args.op == 'find':
        ports = list(list_ports.comports())
        if not ports:
            print("（无串口）")
        for p in ports:
            mark = "  <-- Pico" if (p.vid or 0) == PICO_VID else ""
            print(f"  {p.device}  vid={('%04x' % p.vid) if p.vid else '----'}:pid={('%04x' % p.pid) if p.pid else '----'}  {p.description}{mark}")
        return 0

    pk = Pico(pick_port(args.port))
    try:
        if args.op == 'ping':
            data, msg = pk.cmd("PING", timeout=3.0)
            print("✓ PONG，main.py 存活" if data == b'' and msg == 'OK PONG'
                  else f"回包异常：{data!r} {msg}")
            return 0 if msg == 'OK PONG' else 1
        if args.op == 'act':
            return 0 if do_act(pk, args.ms, args.boot) else 1
        if args.op == 'check':
            ok = wiring_ok(pk)
            print("✓ Pico 自检完成（见上方读数：idle=1 / low=0 / rel=1 为理想）" if ok else "✗ Pico 无响应")
            return 0 if ok else 1
        if args.op == 'jedec':
            if not need_act(pk):
                return 1
            data, msg = pk.cmd("JED", timeout=4.0)
            print(f"JEDEC ID: {data.hex(' ') if data else '-'} {msg or ''}")
            return 0 if len(data) == 3 else 1
        if args.op == 'frd':
            data, msg = do_frd(pk, args.addr, args.n)
            print(f"0x{args.addr:x} +{args.n}B:", data.hex(' ') if data else '-', msg or '')
            return 0 if len(data) == args.n else 1
        if args.op == 'header':
            return do_header(pk)
        if args.op == 'dump':
            return do_dump(pk, args.out, args.chunk, args.fresh)
        if args.op == 'verify':
            return do_verify(pk, args.bin, args.windows)
    finally:
        pk.close()
    return 1


if __name__ == '__main__':
    sys.exit(main())
