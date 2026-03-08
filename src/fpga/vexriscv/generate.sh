#!/bin/bash
# Generate VexiiRiscv with FixedPointMacPlugin for PocketSM64
# Usage: ./generate.sh
#
# Prerequisites: java, sbt, VexiiRiscv repo at $VEXII_REPO

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VEXII_REPO="/home/alberto/Repos/VexiiRiscv"

if [ ! -d "$VEXII_REPO" ]; then
    echo "ERROR: VexiiRiscv repo not found at $VEXII_REPO"
    exit 1
fi

# Copy custom plugin and generation wrapper into VexiiRiscv source tree
cp "$SCRIPT_DIR/FixedPointMacPlugin.scala" \
   "$VEXII_REPO/src/main/scala/vexiiriscv/execute/"
cp "$SCRIPT_DIR/GenPocketSM64.scala" \
   "$VEXII_REPO/src/main/scala/vexiiriscv/"

# Generation flags (same as PocketQuake baseline)
FLAGS=(
    --with-rvm --with-rva --with-rvf --with-rvc
    --with-fetch-l1 --fetch-l1-sets=256 --fetch-l1-ways=1 --fetch-l1-refill-count=2
    --fetch-l1-hardware-prefetch=nl --fetch-axi4
    --with-lsu-l1 --lsu-l1-sets=1024 --lsu-l1-ways=2
    --lsu-l1-refill-count=2 --lsu-l1-writeback-count=2 --lsu-l1-store-buffer-slots=2 --lsu-l1-store-buffer-ops=32
    --lsu-l1-axi4
    --with-btb --btb-sets=512 --relaxed-btb --relaxed-btb-hit
    --with-gshare --with-ras
    --regfile-async --allow-bypass-from=0
    --relaxed-src
    --reset-vector=0
    --region base=0,size=10000,main=0,exe=1
    --region base=10000000,size=4000000,main=1,exe=1
    --region base=20000000,size=10000000,main=0,exe=0
    --region base=30000000,size=8000000,main=1,exe=1
    --region base=38000000,size=8000000,main=0,exe=0
    --region base=40000000,size=10000000,main=0,exe=0
    --region base=50000000,size=4000000,main=0,exe=0
)

echo "Generating VexiiRiscv with FixedPointMacPlugin..."
echo "Flags: ${FLAGS[*]}"

cd "$VEXII_REPO"
sbt "Test/runMain vexiiriscv.GenPocketSM64 ${FLAGS[*]}"

# Copy output
if [ -f "$VEXII_REPO/VexiiRiscv.v" ]; then
    if [ -f "$SCRIPT_DIR/VexiiRiscv_Full.v" ]; then
        cp "$SCRIPT_DIR/VexiiRiscv_Full.v" "$SCRIPT_DIR/VexiiRiscv_Full.v.bak"
        echo "Backed up old VexiiRiscv_Full.v"
    fi
    cp "$VEXII_REPO/VexiiRiscv.v" "$SCRIPT_DIR/VexiiRiscv_Full.v"
    # Copy LUT bin files to both vexriscv/ and project dir (Quartus resolves relative to project)
    cp "$VEXII_REPO"/VexiiRiscv.v_toplevel*.bin "$SCRIPT_DIR/" 2>/dev/null
    cp "$VEXII_REPO"/VexiiRiscv.v_toplevel*.bin "$SCRIPT_DIR/../" 2>/dev/null
    echo "Copied VexiiRiscv.v -> VexiiRiscv_Full.v (+ LUT bins)"
else
    echo "ERROR: VexiiRiscv.v not found after generation"
    exit 1
fi
