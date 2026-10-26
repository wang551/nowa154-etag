#!/bin/bash
# ATC_TLSR_Paper 固件构建（带重试）：循环调用 makeit.exe 直到 ATC_Paper.bin 生成。
#
# 注意：make 不跟踪头文件依赖。改动 *.h 后必须先删除 out/ 和 ATC_Paper.bin
# 强制全量重编，否则产物与旧版 md5 完全相同（等于白编）。
#
# ICE 重试纪律（2026-10-26 实证）：tc32 偶发 internal compiler error，且可能落在
# 写 .o 中途——不清理的增量重试会把半写坏的 .o 当新鲜产物链接（实测产出过 md5
# 异常的"成功"构建）；而每轮全清又把单轮成功率压到 ~1%（27 个文件必须连过）。
# 折中：失败时只删 ICE 对应的那一个 .o，其余成功目标保留。
# 发布前仍须双构建比对 md5。
set -u
FW_DIR="$(cd "$(dirname "$0")/.." && pwd)/ATC_TLSR_Paper/Firmware"
BIN="$FW_DIR/ATC_Paper.bin"
LOG="$(dirname "$0")/build.log"
MAX=200

for i in $(seq 1 $MAX); do
    (cd "$FW_DIR" && ./makeit.exe) > "$LOG" 2>&1
    if grep -q "internal compiler error" "$LOG" || [ ! -f "$BIN" ]; then
        obj="$(grep -oE 'Building file: src/[A-Za-z0-9_./]+\.c' "$LOG" | tail -1 | sed 's|Building file: src/|out/|; s|\.c$|.o|')"
        [ -n "$obj" ] && [ -f "$FW_DIR/$obj" ] && rm -f "$FW_DIR/$obj"
        echo "attempt $i: 失败（已清 $obj），重试..."
    else
        echo "attempt $i: 构建成功"
        ls -la "$BIN"
        md5sum "$BIN"
        exit 0
    fi
done
echo "重试 $MAX 次仍未成功，最后日志在 $LOG"
exit 1
