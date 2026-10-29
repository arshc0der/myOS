#!/bin/bash
# Boot kernel.elf in Bochs with a tiny test loader and read the result from
# the serial port. Usage: tools/bochs/run-bochs-test.sh "<kernel command line>" [seconds]
#   BOCHS=/path/to/bochs   BOCHS_SHARE=/path/to/share/bochs
#   VBE=0x118   set a graphics mode first (tests the framebuffer console)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CMD=${1:-selftest poweroff}
SECS=${2:-60}
BOCHS=${BOCHS:-bochs}
SHARE=${BOCHS_SHARE:-/usr/share/bochs}
W=$ROOT/build/bochs
mkdir -p "$W"
objcopy -O binary "$ROOT/kernel.elf" "$W/kernel.bin"
SIZE=$(stat -c %s "$W/kernel.bin")
KSECT=$(( (SIZE + 511) / 512 ))
ENTRY=$(readelf -h "$ROOT/kernel.elf" | awk '/Entry point/ {print $4}')
VBEDEF=""
[ -n "$VBE" ] && VBEDEF="-DVBE_MODE=$VBE"
nasm -f bin $VBEDEF -DKSECT=$KSECT -DENTRY=$ENTRY -DCMDLINE_TEXT="\"$CMD\"" "$HERE/loader.asm" -o "$W/loader.bin"
python3 - "$W" <<'PY'
import sys
w=sys.argv[1]
img=bytearray(20*16*63*512)
l=open(w+"/loader.bin","rb").read(); k=open(w+"/kernel.bin","rb").read()
img[0:len(l)]=l; img[1536:1536+len(k)]=k
open(w+"/disk.img","wb").write(img)
import os
if not os.path.exists(w+"/data.img"): open(w+"/data.img","wb").write(bytes(8*16*63*512))
PY
sed "s#BOCHS_SHARE#$SHARE#" "$HERE/bochsrc.txt" > "$W/bochsrc.txt"
cd "$W"
rm -f serial.log bochs.log disk.img.lock data.img.lock
# stop early once the kernel reports a result
timeout "$SECS" "$BOCHS" -q -f bochsrc.txt >/dev/null 2>&1 &
BPID=$!
for _ in $(seq 1 "$SECS"); do
    sleep 1
    grep -q -E "SELFTEST: (PASS|FAIL)|KERNEL PANIC" serial.log 2>/dev/null && break
    kill -0 $BPID 2>/dev/null || break
done
kill $BPID 2>/dev/null || true
wait $BPID 2>/dev/null || true
cat serial.log 2>/dev/null || true
