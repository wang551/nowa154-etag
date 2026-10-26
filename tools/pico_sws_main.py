# sws_pico main.py — TLSR8359 SWS 单线主机（Pico RP2040 / MicroPython）
#
# 用法：本文件存为 Pico 的 main.py（tools/pico_sws_host.py 或 _local/pico_up.py 上传）。
#
# 接线（务必核对！触点行：SWS RXD VCC TXD NRST GND，SWS 在最左）：
#   Pico GP2  ↔ 价签 SWS 触点   ← 不要接 VCC，否则每次驱动低都在短接 3V3
#   Pico GND  ↔ 价签 GND
#   Pico 3V3(OUT) ↔ 价签 VCC
#   可选 Pico GP3 ↔ 价签 NRST（RST 命令用）
#
# 时序实现（为什么这么写）：
#   MicroPython 的 Pin.__call__ 单次 ~13.5µs、sleep_us 每次开销 ~19µs，
#   纯 Python 做不出 swire 的 3.4µs 单位波形。故 TX/RX 全部用 @micropython.viper
#   直写 SIO 寄存器 + 读 RP2040 1MHz 定时器，单 cell 精度 ~1µs（实测 29µs vs 21µs，
#   已按固定 4µs/等待 做补偿）。
#
# 两种速率：
#   FAST = 等价 CH340@460800 波形（bit0 低≈2µs/高≈20µs，bit1 低≈17µs/高≈4µs）。
#          标签 ROM 默认 clk_div 下唯一被实证可解码的时序，仅用于软复位/CPU-stop/写 0xb2。
#   SLOW = 单位 U（默认 13µs）。写入 0xb2=DIV_RD(200 → 从机单位 12.5µs) 后所有读写用 SLOW。
#
# 协议（Telink SWire 规格）：
#   帧 = 0x5A(cmd) + addr24 + RW_ID(bit7=1 读) + data + END(0xFF cmd)
#   字节 = 9 cell（cmd flag + 8 数据 MSB first）+ end-low；低宽 >2.5 单位 = 1
#   读 = 主机每字节发 1 单位低脉冲，从机回驱；须等「高 > 低」从机才发
#   写地址自增：flash 命令 4 字节必须各自独立帧；{00,0A} 连写是故意利用自增
#   多字节读须先进 0xb3←0x80 FIFO 模式，读完 0xb3←0x00
#
# 串口命令（115200 行协议）：
#   PING / PIN / ACT[ms] / STOPF[ms] / SRST / RST / SETU<us> / B2<div> / B2F<div> /
#   WR <hexaddr> <hex> / RD <hexaddr> <n> / RAW <hexaddr> <n> / RAWF <addr> <n> /
#   PROBE [addr] [ms] / FRD <hexaddr> <n> / JED

import gc
import sys
import utime
import micropython
from machine import Pin

SWS_GP, RST_GP = 2, 3
DIV_RD = 200         # 从机 clk_div（单位 = 200/16MHz = 12.5µs）
U = 13               # SLOW 单位 µs（主机）
UNIT_RD = 12         # 从机单位 µs（DIV_RD/16 取整）

sws = Pin(SWS_GP, Pin.OPEN_DRAIN, Pin.PULL_UP, value=1)
rst = Pin(RST_GP, Pin.OUT, value=1)

# 焊盘：DRIVE=11(12mA) + IE + 上拉 + 施密特，**OD 必须为 0**（RP2040 的 OD 是"输出禁用"，
# 置 1 会让引脚彻底失去驱动能力）。开漏靠 SIO 的 OE 位模拟：OE=1 驱动低，OE=0 高阻。
# 0xFA = OD0|IE1|PUE1|SCHMITT1|DRIVE11。弱驱动(0x7A=2mA)时 4µs 高电平爬不过线电容(~30µs
# 才到 VIH)，从机位判决全错——这是先前写帧全部无效的根因（CH340 是 10-20mA 强推挽）。
try:
    import machine as _m
    _m.mem32[0x4001C004 + SWS_GP * 4] = 0xFA
except Exception:
    pass

t_us = utime.ticks_us
tdiff = utime.ticks_diff
t_ms = utime.ticks_ms

gc.collect()
gc.disable()

# SIO 直写寄存器（GPIO2 → 掩码 4）
_SIO_CLR = 0xD0000018
_SIO_SET = 0xD0000014
_SIO_IN = 0xD0000004
_TIMER = 0x40054000   # TIMER0 基址；[10] = TIMERAWL（µs）
_OV = 4               # 每次等待的固定超调补偿（µs，实测）

_ws = bytearray(1600)  # RX cell 低宽 / 抓波形的跳变间隔（µs，钳到 255）


# ---------------------------------------------------------------- viper 时序层

@micropython.viper
def _tx_cell_v(bit: int, lo: int, hi: int):
    """推挽 cell（与 CH340 TX 一致）：低 lo µs → 高 hi µs。
    开漏的弱上拉在有几十 pF 负载时爬不到 VIH，写路径必须推挽；
    读时钟在 _rx_byte_v 里单独用 OE 释放。"""
    clr = ptr32(0xD0000018)
    st = ptr32(0xD0000014)
    oes = ptr32(0xD0000024)
    tm = ptr32(0x40054000)
    if lo > 5:
        lo = lo - 4
    else:
        lo = 1
    if hi > 5:
        hi = hi - 4
    else:
        hi = 1
    clr[0] = 4
    oes[0] = 4
    if lo > 1:
        t = tm[10]
        while (tm[10] - t) < lo:
            pass
    st[0] = 4
    t = tm[10]
    while (tm[10] - t) < hi:
        pass


@micropython.viper
def _tx_byte_v(v: int, cmd: int, lo0: int, hi0: int, lo1: int, hi1: int):
    """1 个 swire 字节 = [cmd cell][8 数据 cell MSB first][结束 cell]。
    结束 cell 形状同 bit0（低1单位+高4单位），对应 atc 编码器每字节第 10 个 UART 字节 0xFE；
    少了它从机无法切分字节边界（这是先前写/读全静默的根因）。"""
    clr = ptr32(0xD0000018)
    st = ptr32(0xD0000014)
    oes = ptr32(0xD0000024)
    tm = ptr32(0x40054000)
    for i in range(10):
        if i == 0:
            bit = cmd
            lo = lo1
            hi = hi1
        elif i == 9:
            bit = 0
            lo = lo0
            hi = hi0
        else:
            bit = (v >> (8 - i)) & 1
            if bit:
                lo = lo1
                hi = hi1
            else:
                lo = lo0
                hi = hi0
        clr[0] = 4
        oes[0] = 4
        if lo > 0:
            t = tm[10]
            while (tm[10] - t) < lo:
                pass
        st[0] = 4
        t = tm[10]
        while (tm[10] - t) < hi:
            pass


@micropython.viper
def _rx_byte_v(buf: ptr8, clk: int, unit: int, tmo: int) -> int:
    """发 1 个读时钟，采 9 cell + end-low 写进 buf；返回字节，失败 -1"""
    clr = ptr32(0xD0000018)
    oes = ptr32(0xD0000024)
    oec = ptr32(0xD0000028)
    tm = ptr32(0x40054000)
    gi = ptr32(0xD0000004)
    thr = (unit * 5) // 2
    if clk > 5:
        clk = clk - 4
    else:
        clk = 1
    clr[0] = 4
    oes[0] = 4
    t0 = tm[10]
    while (tm[10] - t0) < clk:
        pass
    oec[0] = 4
    v = 0
    for i in range(9):
        t = tm[10]
        while (gi[0] & 4) != 0:
            if (tm[10] - t) > tmo:
                return -1
        t1 = tm[10]
        while (gi[0] & 4) == 0:
            if (tm[10] - t1) > tmo:
                return -1
        d = tm[10] - t1
        if d > 250:
            d = 250
        buf[i] = d
        if i > 0:
            v = v * 2
            if d > thr:
                v = v + 1
    t = tm[10]
    while (gi[0] & 4) != 0:          # 等从机 end-low 出现，框定字节边界
        if (tm[10] - t) > tmo:
            buf[9] = 0
            return v
    t1 = tm[10]
    while (gi[0] & 4) == 0:
        if (tm[10] - t1) > tmo:
            break
    d = tm[10] - t1
    if d > 250:
        d = 250
    buf[9] = d
    return v


# ---------------------------------------------------------------- 模式与打包

LO0, HI0, LO1, HI1, CLK = 2, 19, 17, 4, 3     # FAST 默认
_FAST = True
_CUSTOM = False                              # SETFAST 之后 _mode(True) 不再覆盖参数
_C = (1, 16, 14, 2)                          # SETFAST 保存的自定义 FAST 参数


def _wait(us):
    """包间隔用的微秒忙等（勿用 sleep_us，本固件上每次调用 ~19µs 开销）"""
    dl = t_us() + us
    while tdiff(t_us(), dl) < 0:
        pass


def _mode(fast):
    global LO0, HI0, LO1, HI1, CLK, _FAST
    _FAST = fast
    if fast:
        if _CUSTOM:
            LO0, HI0, LO1, HI1 = _C[0], _C[1], _C[2], _C[3]
        else:
            LO0, HI0, LO1, HI1, CLK = 1, 16, 14, 2, 2
    else:
        LO0, HI0, LO1, HI1 = U, 4 * U, 4 * U, U
        CLK = max(6, U // 2)


def _byte(v, cmd):
    _tx_byte_v(v & 0xFF, 1 if cmd else 0, LO0, HI0, LO1, HI1)


def _set_clk(us):
    global CLK
    CLK = max(1, min(200, us))


def _mode_unit(u):
    """按从机单位 u（µs）生成整条链路时序：bit0 低 u/高 4u，bit1 低 4u/高 u，读时钟 u"""
    global LO0, HI0, LO1, HI1, CLK, _FAST
    _FAST = False
    LO0, HI0, LO1, HI1 = u, 4 * u, 4 * u, u
    CLK = max(2, u)


def sweep(per_ms=400):
    """只读速度扫描：对每个候选从机单位发读命令，看谁应答。返回 (单位, cell 宽度列表)"""
    res = []
    for u in (2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25, 32, 40):
        _mode_unit(u)
        t0 = t_ms()
        n = 0
        while tdiff(t_ms(), t0) < per_ms:
            v = _rx_byte_v(_ws, CLK, u, 3000)
            n += 1
            if v >= 0:
                res.append((u, v, list(_ws[:10]), n))
                break
    _mode(False)
    return res


def stopsweep(per_ms=1500):
    """写+读联合扫描：每个候选单位先用「1U 低+4U 高 / 4U 低+1U 高」的标准波形连发 CPU-stop，
    再用同一单位读回 0x602。命中即说明从机单位就是它。"""
    out = []
    for u in (2, 3, 4, 5, 6, 7, 9, 12):
        _mode_unit(u)
        t0 = t_ms()
        n = 0
        while tdiff(t_ms(), t0) < per_ms:
            wr(0x0602, b'\x05')
            n += 1
        _hdr(0x0602, 0x80)
        v = _rx_byte_v(_ws, u, u, 20000)
        _end()
        out.append((u, v, list(_ws[:10]), n))
        if v >= 0:
            break
    _mode(False)
    return out


def _hdr(addr, rw):
    # 注意：参考实现（CH340 抓包实测）每帧是直接从 START 字节开始，前面没有额外空闲 cell
    _byte(0x5A, True)
    _byte((addr >> 16) & 0xFF, False)
    _byte((addr >> 8) & 0xFF, False)
    _byte(addr & 0xFF, False)
    _byte(rw, False)


def _end():
    _byte(0xFF, True)


def wr(addr, data):
    _hdr(addr, 0x00)
    for b in data:
        _byte(b, False)
    _end()


def wr_fast(addr, data):
    _mode(True)
    wr(addr, data)
    _mode(False)


def rd(addr, n):
    if n > 1:
        wr(0x00B3, b'\x80')                  # FIFO 模式锁地址
    _hdr(addr, 0x80)
    out = bytearray()
    for _ in range(n):
        v = _rx_byte_v(_ws, CLK, UNIT_RD, 20000)
        if v < 0:
            break
        out.append(v)
    _end()
    if n > 1:
        wr(0x00B3, b'\x00')
    return bytes(out)


def rd_raw(addr, n):
    """返回每字节的 cell 低宽列表（含 end-low）"""
    if n > 1:
        wr(0x00B3, b'\x80')
    _hdr(addr, 0x80)
    out = []
    for _ in range(n):
        v = _rx_byte_v(_ws, CLK, UNIT_RD, 20000)
        out.append(list(_ws[:10]))
        if v < 0:
            break
    _end()
    if n > 1:
        wr(0x00B3, b'\x00')
    return out


# ---------------------------------------------------------------- 激活 / 复位

def soft_reset():
    wr_fast(0x006F, b'\x20')


def hard_rst():
    rst(0)
    utime.sleep_ms(100)
    rst(1)


def _stop_spam(ms, do_reset=True, gap_us=0):
    if do_reset:
        soft_reset()
    _mode(True)
    t0 = t_ms()
    n = 0
    while tdiff(t_ms(), t0) < ms:
        wr(0x0602, b'\x05')
        n += 1
        if gap_us:
            _wait(gap_us)
    _mode(False)
    return n


def activate(ms=3000):
    n = _stop_spam(ms)
    _mode(True)
    wr(0x00B2, bytes([DIV_RD]))              # 从机此时仍是默认速率 → 必须 FAST 发
    _mode(False)
    return rd(0x0602, 1), n


def winrd(addr, n, ms=3000):
    """窗口读：CPU-stop 连发抓激活窗，连发一停立刻（同一激活态内）发读命令采样。
    关键认知（2026-10-26 实测）：连发一停 SWS 从机也哑，激活与读写必须同一节奏。"""
    _mode(True)
    t0 = t_ms()
    nwr = 0
    while tdiff(t_ms(), t0) < ms:
        wr(0x0602, b'\x05')
        nwr += 1
    if n > 1:
        wr(0x00B3, b'\x80')
    _hdr(addr, 0x80)
    got = bytearray()
    for _ in range(n):
        v = _rx_byte_v(_ws, CLK, 12, 20000)
        if v < 0:
            break
        got.append(v)
    _end()
    if n > 1:
        wr(0x00B3, b'\x00')
    _mode(False)
    return bytes(got), nwr


def winraw(addr, n, ms=1500, clk=None):
    """窗口 RAW：连发抓窗后立刻读，返回 cell 低宽原始数据（看从机到底回没回）"""
    _mode(True)
    t0 = t_ms()
    while tdiff(t_ms(), t0) < ms:
        wr(0x0602, b'\x05')
    if clk:
        _set_clk(clk)
    if n > 1:
        wr(0x00B3, b'\x80')
    _hdr(addr, 0x80)
    out = []
    for _ in range(n):
        v = _rx_byte_v(_ws, CLK, 12, 20000)
        out.append(list(_ws[:10]))
        if v < 0:
            break
    _end()
    if n > 1:
        wr(0x00B3, b'\x00')
    _mode(False)
    return out
    """窗口读：CPU-stop 连发抓激活窗，连发一停立刻（同一激活态内）发读命令采样。
    关键认知（2026-10-26 实测）：连发一停 SWS 从机也哑，激活与读写必须同一节奏。"""
    _mode(True)
    t0 = t_ms()
    nwr = 0
    while tdiff(t_ms(), t0) < ms:
        wr(0x0602, b'\x05')
        nwr += 1
    if n > 1:
        wr(0x00B3, b'\x80')
    _hdr(addr, 0x80)
    got = bytearray()
    for _ in range(n):
        v = _rx_byte_v(_ws, CLK, 12, 20000)
        if v < 0:
            break
        got.append(v)
    _end()
    if n > 1:
        wr(0x00B3, b'\x00')
    _mode(False)
    return bytes(got), nwr


def bootstop(ms=1500, gap_us=0):
    """硬复位（GP3→NRST）→ 立刻 CPU-stop 连发，抓启动窗口。
    这是 atc 网页烧录器 Activate() 里 RTS 复位那一步的对应动作。"""
    hard_rst()
    return _stop_spam(ms, False, gap_us)


def bootact(ms=1500):
    """完全镜像 atc Activate()：硬复位 → 写 0x06f 软复位 → CPU-stop 连发 → 默认速率读 0x602"""
    hard_rst()
    _mode(True)
    wr(0x006F, b'\x20')                      # SoftResetMSU（参考实现复位后的第一帧）
    n = _stop_spam(ms, False)
    _mode(True)
    v = rd(0x0602, 1)
    v2 = b''
    if v == b'\x05':
        wr(0x00B2, bytes([DIV_RD]))          # 此刻从机仍是默认速率 → FAST 发
        _mode(False)
        v2 = rd(0x0602, 1)
    _mode(False)
    return v, v2, n


def _line_read():
    import machine as _m
    return 1 if (_m.mem32[0xD0000004] & 4) else 0


def _line_drive_low():
    import machine as _m
    _m.mem32[0xD0000018] = 4        # 输出锁存 0
    _m.mem32[0xD0000024] = 4        # OE 使能 → 真正拉低


def _line_release():
    import machine as _m
    _m.mem32[0xD0000028] = 4        # OE 关闭 → 高阻


def pin_test():
    """电气自检：空闲高 / 开漏拉低（2ms 短脉冲） / 释放回高"""
    _line_release()
    utime.sleep_ms(5)
    idle = _line_read()
    _line_drive_low()
    utime.sleep_ms(2)
    low = _line_read()
    _line_release()
    utime.sleep_ms(5)
    rel = _line_read()
    return idle, low, rel


@micropython.viper
def _txcap_v(buf: ptr8, blen: int, pkt: ptr8, n: int, flags: int,
             lo0: int, hi0: int, lo1: int, hi1: int) -> int:
    """发 n 个字节并记录每 cell 的实际 (低,高) 时长（µs）"""
    clr = ptr32(0xD0000018)
    st = ptr32(0xD0000014)
    oes = ptr32(0xD0000024)
    tm = ptr32(0x40054000)
    k = 0
    for b in range(n):
        v = pkt[b]
        cmdf = (flags >> b) & 1
        for i in range(10):
            if i == 0:
                bit = cmdf
                lo = lo1
                hi = hi1
            elif i == 9:
                bit = 0
                lo = lo0
                hi = hi0
            else:
                bit = (v >> (8 - i)) & 1
                if bit:
                    lo = lo1
                    hi = hi1
                else:
                    lo = lo0
                    hi = hi0
            t0 = tm[10]
            clr[0] = 4
            oes[0] = 4
            if lo > 0:
                while (tm[10] - t0) < lo:
                    pass
            t1 = tm[10]
            st[0] = 4
            while (tm[10] - t1) < hi:
                pass
            t2 = tm[10]
            if k < blen - 1:
                d = t1 - t0
                buf[k] = d if d < 255 else 255
                k = k + 1
                d = t2 - t1
                buf[k] = d if d < 255 else 255
                k = k + 1
    return k


def txcap():
    """自发自测：发 CPU-stop 帧并记录实测 cell 时长"""
    pkt = bytearray([0x5A, 0x00, 0x06, 0x02, 0x00, 0x05, 0xFF])
    _mode(True)
    k = _txcap_v(_ws, len(_ws), pkt, 7, 0x41, LO0, HI0, LO1, HI1)
    _mode(False)
    return k, bytes(_ws[:max(0, k)]).hex()


@micropython.viper
def _tcap_v(buf: ptr8, n: int, wait_ms: int) -> int:
    """等线出现低电平（外部主机开始发帧）后，记录 n 个相邻跳变间隔（µs，钳 255）"""
    gi = ptr32(0xD0000004)
    tm = ptr32(0x40054000)
    t0 = tm[10]
    while (gi[0] & 4) != 0:
        if (tm[10] - t0) > wait_ms * 1000:
            return -1
    t1 = tm[10]
    last = 0
    k = 0
    while k < n:
        v = (gi[0] >> 2) & 1
        if v != last:
            d = tm[10] - t1
            if d > 255:
                d = 255
            if k < n:
                buf[k] = d
                k = k + 1
            t1 = tm[10]
            last = v
    return k


def tcap(wait_ms=60000, n=1500):
    _line_release()
    utime.sleep_ms(5)
    k = _tcap_v(_ws, min(n, len(_ws)), wait_ms)
    return k, bytes(_ws[:max(0, k)]).hex() if k > 0 else ''


@micropython.viper
def _capture_v(buf: ptr8, n: int, ms: int) -> int:
    """高阻监听 SWS 线，记录相邻电平跳变的间隔（µs，钳到 255）。"""
    gi = ptr32(0xD0000004)
    tm = ptr32(0x40054000)
    t0 = tm[10]
    t1 = t0
    last = (gi[0] >> 2) & 1
    k = 0
    while (tm[10] - t0) < ms * 1000:
        v = (gi[0] >> 2) & 1
        if v != last:
            d = tm[10] - t1
            if d > 255:
                d = 255
            if k < n:
                buf[k] = d
                k = k + 1
            t1 = tm[10]
            last = v
    return k


def capture(ms=5000, n=300):
    """被动抓波形：GP2 高阻，只记录外部（CH340）驱动的跳变"""
    _line_release()
    utime.sleep_ms(20)
    start = _line_read()
    k = _capture_v(_ws, min(n, len(_ws)), ms)
    return start, bytes(_ws[:k]).hex()


@micropython.viper
def _watch_pin(ms: int) -> int:
    gi = ptr32(0xD0000004)
    tm = ptr32(0x40054000)
    t0 = tm[10]
    last = (gi[0] >> 2) & 1
    n = 0
    while (tm[10] - t0) < ms * 1000:
        v = (gi[0] >> 2) & 1
        if v != last:
            n = n + 1
            last = v
    return n


def nrst_watch(ms=500, hold_ms=100):
    """GP2 高阻监听 + GP3(NRST) 复位脉冲：标签启动若初始化 PA7，会看到跳变。
    用来同时验证 a) GP3→NRST 是否真的接了 b) GP2 是否真的在 PA7/SWS 上。"""
    _line_release()
    utime.sleep_ms(20)
    before = _watch_pin(100)
    rst(0)
    utime.sleep_ms(hold_ms)
    rst(1)
    n = _watch_pin(ms)
    return before, n



# ---------------------------------------------------------------- flash 桥

def _flash_cmd_read(cmd_bytes, n):
    wr(0x0D, b'\x00')                        # CSN 低
    for b in cmd_bytes:
        wr(0x0C, bytes([b]))                 # 每字节独立帧（写地址自增！）
    wr(0x0C, b'\x00\x0A')                    # 0x0c←00 触发首读；0x0A 落 0x0d = auto-read
    d = rd(0x0C, n)
    wr(0x0D, b'\x01')                        # CSN 高
    return d


def flash_jedec():
    return _flash_cmd_read((0x9F,), 3)


def flash_read(addr, n):
    return _flash_cmd_read((0x03, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF), n)


# ---------------------------------------------------------------- 主循环

def main():
    global U, UNIT_RD, DIV_RD, LO0, HI0, LO1, HI1, _FAST, _CUSTOM
    _mode(False)
    print("SWS-PICO ready: PING/PIN/ACT[ms]/STOPF[ms]/SRST/RST/SETU/B2/B2F/WR/RD/RAW/RAWF/PROBE/FRD/JED")
    while True:
        line = sys.stdin.readline()
        if not line:
            utime.sleep_ms(5)
            continue
        p = line.split()
        if not p:
            continue
        try:
            c = p[0].upper()
            if c == 'PING':
                print("OK PONG")
            elif c == 'PIN':
                a, b, c2 = pin_test()
                print("OK PIN idle=%d low=%d rel=%d" % (a, b, c2))
            elif c == 'SETFAST':
                global _C
                LO0, HI0, LO1, HI1 = (int(p[1]), int(p[2]), int(p[3]), int(p[4]))
                _C = (LO0, HI0, LO1, HI1)
                _FAST = True
                _CUSTOM = True
                print("OK SETFAST", LO0, HI0, LO1, HI1)
            elif c == 'BENCH':
                n = int(p[1]) if len(p) > 1 else 200
                t0 = t_us()
                for _ in range(n):
                    _byte(0x5A, True)
                dt = tdiff(t_us(), t0)
                print("OK BENCH us_per_byte=%d (CH340 实测 205)" % (dt // n))
            elif c == 'PAD':
                import machine as _mm
                _v = int(p[1], 16)
                _mm.mem32[0x4001C004 + SWS_GP * 4] = _v
                print("OK PAD %02x" % _v)
            elif c == 'TXCAP':
                k, hexs = txcap()
                print("OK TXCAP n=%d d=%s" % (k, hexs))
            elif c == 'WINRD':
                d, nwr = winrd(int(p[1], 16) if len(p) > 1 else 0x602,
                               int(p[2]) if len(p) > 2 else 1,
                               int(p[3]) if len(p) > 3 else 3000)
                print("OK WINRD", d.hex() if d else '-', "nwr=%d" % nwr)
            elif c == 'WINRAW':
                r = winraw(int(p[1], 16) if len(p) > 1 else 0x602,
                           int(p[2]) if len(p) > 2 else 2,
                           int(p[3]) if len(p) > 3 else 1500,
                           int(p[4]) if len(p) > 4 else None)
                print("OK WINRAW", ';'.join(','.join(str(w) for w in ws) for ws in r))
            elif c == 'TCAP':
                k, hexs = tcap(int(p[1]) if len(p) > 1 else 60000,
                               int(p[2]) if len(p) > 2 else 1500)
                print("OK TCAP n=%d d=%s" % (k, hexs))
            elif c == 'CAPTURE':
                st, hexs = capture(int(p[1]) if len(p) > 1 else 5000,
                                   int(p[2]) if len(p) > 2 else 300)
                print("OK CAPTURE start=%d d=%s" % (st, hexs))
            elif c == 'NRSTWATCH':
                a, b = nrst_watch(int(p[1]) if len(p) > 1 else 500,
                                  int(p[2]) if len(p) > 2 else 100)
                print("OK NRSTWATCH idle_changes=%d boot_changes=%d" % (a, b))
            elif c == 'ACT':
                v, n = activate(int(p[1]) if len(p) > 1 else 3000)
                print("OK", v.hex() if v else '-', "n=%d" % n)
            elif c == 'STOPF':
                ms = int(p[1]) if len(p) > 1 else 3000
                gap = int(p[2]) if len(p) > 2 else 0
                rst = (int(p[3]) if len(p) > 3 else 0) != 0
                print("OK STOPF n=%d" % _stop_spam(ms, rst, gap))
            elif c == 'SRST':
                soft_reset()
                print("OK SRST")
            elif c == 'RST':
                hard_rst()
                print("OK RST")
            elif c == 'BOOTSTOP':
                n = bootstop(int(p[1]) if len(p) > 1 else 1500,
                             int(p[2]) if len(p) > 2 else 0)
                print("OK BOOTSTOP n=%d" % n)
            elif c == 'BOOTACT':
                v, v2, n = bootact(int(p[1]) if len(p) > 1 else 1500)
                print("OK %s n=%d slow=%s" % (v.hex() if v else '-', n,
                                              v2.hex() if v2 else '-'))
            elif c == 'SETU':
                U = max(6, min(60, int(p[1])))
                UNIT_RD = max(4, DIV_RD // 16)
                _mode(False)
                print("OK SETU", U, UNIT_RD)
            elif c == 'B2':
                DIV_RD = int(p[1]) & 0xFF
                wr(0x00B2, bytes([DIV_RD]))
                UNIT_RD = max(4, DIV_RD // 16)
                U = max(6, UNIT_RD + 1)
                _mode(False)
                print("OK B2", DIV_RD, "U=", U)
            elif c == 'B2F':
                d = int(p[1]) & 0xFF
                wr_fast(0x00B2, bytes([d]))
                print("OK B2F", d)
            elif c == 'WR':
                wr(int(p[1], 16), bytes.fromhex(''.join(p[2:])))
                print("OK WR")
            elif c == 'RD':
                print("OK RD", rd(int(p[1], 16), int(p[2])).hex())
            elif c == 'RAW':
                r = rd_raw(int(p[1], 16), int(p[2]))
                print("OK RAW", ';'.join(','.join(str(w) for w in ws) for ws in r))
            elif c == 'RAWF':
                _mode(True)
                if len(p) > 3:                 # 可选读时钟宽度（µs）
                    _set_clk(int(p[3]))
                r = rd_raw(int(p[1], 16), int(p[2]))
                _mode(False)
                print("OK RAW", ';'.join(','.join(str(w) for w in ws) for ws in r))
            elif c == 'PROBE':
                addr = int(p[1], 16) if len(p) > 1 else 0x602
                ms = int(p[2]) if len(p) > 2 else 5000
                t0 = t_ms()
                n = 0
                hit = []
                _mode(True)
                while tdiff(t_ms(), t0) < ms:
                    v = _rx_byte_v(_ws, CLK, 12, 2000)
                    n += 1
                    if v >= 0:
                        hit = list(_ws[:10])
                        break
                _mode(False)
                print("OK PROBE n=%d" % n, ','.join(str(w) for w in hit) if hit else '-')
            elif c == 'SWEEP':
                r = sweep(int(p[1]) if len(p) > 1 else 400)
                if not r:
                    print("OK SWEEP none")
                else:
                    for u, v, ws, n in r:
                        print("OK SWEEP u=%d val=%02x n=%d ws=%s" % (u, v, n, ','.join(str(w) for w in ws)))
            elif c == 'STOPSWEEP':
                for u, v, ws, n in stopsweep(int(p[1]) if len(p) > 1 else 1500):
                    print("OK STOPSWEEP u=%d val=%s n=%d ws=%s"
                          % (u, ('%02x' % v) if v >= 0 else '-', n,
                             ','.join(str(w) for w in ws)))
            elif c == 'FRD':
                print("OK FRD", flash_read(int(p[1], 16), int(p[2])).hex())
            elif c == 'JED':
                print("OK JED", flash_jedec().hex())
            else:
                print("ERR cmd")
        except Exception as e:
            print("ERR", e)
        gc.collect()


main()
