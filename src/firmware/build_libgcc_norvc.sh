#!/bin/bash
# Build libgcc_norvc.a: a version of libgcc without compressed instructions.
#
# The rv32imafc/ilp32f multilib libgcc contains compressed (RVC) instructions.
# Since VexRiscv is built with compressedGen=false for higher Fmax, we need a
# libgcc without RVC.  We take the rv32im/ilp32 version (which has no RVC)
# and patch its ELF flags to indicate single-float ABI (ilp32f) so the linker
# accepts it.
#
# The __extendsfdf2 and __truncdfsf2 functions are ABI-incompatible between
# ilp32 and ilp32f (float args/returns go in different registers), so those
# are provided by libgcc_norvc.c compiled with the correct ABI.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR=$(mktemp -d)
trap "rm -rf $WORK_DIR" EXIT

LIBGCC_SRC="/usr/lib/gcc/riscv64-elf/15.2.0/rv32im/ilp32/libgcc.a"

if [ ! -f "$LIBGCC_SRC" ]; then
    echo "ERROR: $LIBGCC_SRC not found"
    exit 1
fi

echo "Extracting objects from $LIBGCC_SRC..."
cd "$WORK_DIR"
riscv64-elf-ar x "$LIBGCC_SRC"

echo "Patching ELF flags (soft-float → single-float ABI)..."
for f in *.o; do
    python3 -c "
import sys
with open(sys.argv[1], 'r+b') as f:
    f.seek(0x24)
    flags = int.from_bytes(f.read(4), 'little')
    flags = (flags & ~0x7) | 0x2  # clear RVC, set single-float ABI
    f.seek(0x24)
    f.write(flags.to_bytes(4, 'little'))
" "$f"
done

echo "Building archive..."
riscv64-elf-ar rcs "$SCRIPT_DIR/libgcc_norvc.a" *.o

echo "Built $SCRIPT_DIR/libgcc_norvc.a"
echo "Verify: zero compressed instructions:"
COUNT=$(riscv64-elf-objdump -d "$SCRIPT_DIR/libgcc_norvc.a" 2>/dev/null | grep -cP '^\s+[0-9a-f]+:\s+[0-9a-f]{4}\s+\S' || true)
echo "  Compressed instruction count: $COUNT"
