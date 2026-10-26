#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 FM11NC08I 日志区 dump（0x41000 起的 256B/页环形区，v27+ 结构版本 9）。

用法: python tools/nfc_log_parse.py <dump.bin> [--t4t]
       python tools/nfc_log_parse.py <dump.bin> --bsm
  默认打印全部非空页摘要；--t4t 只打印含 T4T 交换记录（p[59]=0xFD）的页。
  --bsm 解析 v36 盲态机时间线区（0x46000 dump）：每页 'BS' 头 + 3B 事件流。

页布局（nfc.c nfc_log_capture）：
  [0..3] 时间戳  [4] 标志  [5] 序号  [6] IRQ 电平  [7] 卡死计数
  [8..20] 寄存器 0xFFF1-0xFFFD（[8]=FIFO_FLUSH [9]=FifoCnt [10]=RfStatus
           [11]=TxEn [14]=MainIrq 服务轮原值 [23]=FifoIrqMask...）
  [21] FIFO 计数  [54] retention 唤醒数  [55..58] 原始 tick
  T4T 交换记录（盲应答/探针页）：
  [59]=0xFD 标记  [60..71]=12 次单字节 FIFO 读（v34 探针）
  [72..83]=12B 突发读  [84]=盲状态机 state  [92]=resp_len  [93..125]=resp
  p[4..7]='TRC1' = 轨迹转储页（24 条 [tick4 irq1 mirq1 cnt1 st1 pad2]）
"""

import os
import sys

# 安全约束：只接受当前目录下的相对路径（禁止绝对路径与 ../ 穿越）
def safe_read(path):
    norm = path.replace("\\", "/")
    if os.path.isabs(norm) or ".." in norm.split("/"):
        raise SystemExit(f"非法路径 {path!r}：仅允许当前目录下的相对路径")
    return open(path, "rb").read()

REG_NAMES = ["Flush", "FifoCnt", "RfStatus", "TxEn", "Baud?", "Rats",
             "MainIrq", "FifoIrq", "AuxIrq", "MIM", "FIM", "AIM", "rFFFD"]


def parse_page(p, page_no, only_t4t=False):
    if p[4] == ord('T') and p[5] == ord('R') and p[6] == ord('C') and p[7] == ord('1'):
        if only_t4t:
            return None
        out = [f"p{page_no:02d} TRC1 轨迹"]
        for i in range(24):
            e = p[8 + i * 10:8 + i * 10 + 10]
            tick = int.from_bytes(e[0:4], "little")
            out.append(f"  t={tick:08x} irq={e[4]} mirq={e[5]:02x} cnt={e[6]:02x} st={e[7]:02x}")
        return "\n".join(out)
    empty = all(b == 0xFF for b in p) or all(b == 0x00 for b in p)
    if empty:
        return None
    ts = int.from_bytes(p[0:4], "little")
    flags = p[4]
    t4t = p[59] == 0xFD
    if only_t4t and not t4t:
        return None
    regs = " ".join(f"{n}={p[8+i]:02x}" for i, n in enumerate(REG_NAMES))
    lines = [f"p{page_no:02d} seq={p[5]:3d} f={flags:02x} irq={p[6]} ts={ts:08x} tick={int.from_bytes(p[55:59], 'little'):08x} | {regs}"]
    if t4t:
        sb = p[60:72].hex(" ")
        burst = p[72:84].hex(" ")
        st = p[84]
        rlen = p[92]
        resp = p[93:93 + min(rlen, 33)].hex(" ")
        lines.append(f"      单字节×12: {sb}")
        lines.append(f"      突发×12 : {burst}")
        lines.append(f"      盲态={st} resp_len={rlen} resp={resp}")
        # 判决辅助：单字节序列里非填充字节数
        filler = {0xC0, 0xC7, 0x6A, 0xFF, 0x00}
        real = [f"{i}:{b:02x}" for i, b in enumerate(p[60:72]) if b not in filler]
        lines.append(f"      非填充单字节: {real if real else '无（全是填充码）'}")
    return "\n".join(lines)


ST_NAMES = {0: "SELECT app→9000", 1: "SELECT CC→9000", 2: "READ CC→CC内容",
            3: "SELECT file→9000", 4: "READ NLEN", 5: "READ data→NDEF文本",
            0xFC: "[重传]原样重发", 0xFD: "[间隔]>1.5s会话重置", 0xFE: "[重激活]RATS",
            0xF3: "[TxDone]芯片自发射!!", 0xF4: "[Arbit]仲裁事件"}


def parse_bsm(data):
    pages = [data[i * 256:(i + 1) * 256] for i in range(len(data) // 256)]
    ev_no = 0
    for no, p in enumerate(pages):
        if p[0:2] != b"BS":
            if any(p):
                print(f"p{no:02d} 非 BS 页（残留/空）")
            continue
        n, seq = p[2], p[3]
        total = int.from_bytes(p[4:6], "little")
        print(f"--- 页 {no:02d} 事件数={n} 页序={seq} 累计={total} ---")
        for i in range(n):
            st, pcb, rlen = p[8 + i * 3], p[9 + i * 3], p[10 + i * 3]
            name = ST_NAMES.get(st, f"st={st}→兜底全文件" if st >= 6 else f"st={st}")
            print(f"  #{ev_no:4d} {name:24s} pcb={pcb:02x} rlen={rlen}")
            ev_no += 1
    print(f"--- 共 {ev_no} 事件 ---")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    data = safe_read(args[0])
    if "--bsm" in sys.argv:
        parse_bsm(data)
        return
    only_t4t = "--t4t" in sys.argv
    pages = [data[i * 256:(i + 1) * 256] for i in range(len(data) // 256)]
    meta = pages[0]
    if meta[0:4] == b"NFCL":
        print(f"meta: 版本={meta[4]} 回卷代数={meta[5]} 布防时刻={int.from_bytes(meta[6:8], 'little') & 0xffff:04x}")
        print(f"meta[0x40..0x4F]: 3B5={meta[0x40]:02x} 补写={meta[0x41]} 回读={meta[0x42]:02x} "
              f"flushCnt={meta[0x43]:02x} IRQ前={meta[0x44]} MI={meta[0x45]:02x} "
              f"IRQ中={meta[0x46]} FifoIrq={meta[0x47]:02x} AuxIrq={meta[0x48]:02x} "
              f"IRQ后={meta[0x49]} IRQ末={meta[0x4A]} FFE0/1/2={meta[0x4B]:02x}/{meta[0x4C]:02x}/{meta[0x4D]:02x} "
              f"MIM={meta[0x4E]:02x} v28标记={meta[0x4F]}")
    else:
        print(f"meta 魔数不符: {meta[0:4]!r}（可能是空区/旧固件残留）")
    n_shown = 0
    for no, p in enumerate(pages[1:], 1):
        s = parse_page(p, no, only_t4t)
        if s:
            print(s)
            n_shown += 1
    print(f"--- 共 {n_shown} 页有效记录 ---")


if __name__ == "__main__":
    main()
