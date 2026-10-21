#!/bin/bash
# ATC_TLSR_Paper 固件构建（带重试）。
# tc32-elf-gcc 4.5.1 偶发 "internal compiler error: in decl_attributes"（约 10-20% 概率、
# 随机文件），增量重编即可收敛，故循环调用 makeit.exe 直到 ATC_Paper.bin 生成。
#
# 注意：make 不跟踪头文件依赖。改动 *.h 后必须先删除 out/ 和 ATC_Paper.bin
# 强制全量重编，否则产物与旧版完全相同（md5 都不变）。
set -u
FW_DIR="$(cd "$(dirname "$0")/.." && pwd)/ATC_TLSR_Paper/Firmware"
BIN="$FW_DIR/ATC_Paper.bin"
LOG="$(dirname "$0")/build.log"
MAX=15

for i in $(seq 1 $MAX); do
    (cd "$FW_DIR" && ./makeit.exe) > "$LOG" 2>&1
    if grep -q "internal compiler error" "$LOG" || [ ! -f "$BIN" ]; then
        echo "attempt $i: 失败($(grep -c 'internal compiler error' "$LOG") 个 ICE)，重试..."
    else
        echo "attempt $i: 构建成功"
        ls -la "$BIN"
        md5sum "$BIN"
        exit 0
    fi
done
echo "重试 $MAX 次仍未成功，最后日志在 $LOG"
exit 1
