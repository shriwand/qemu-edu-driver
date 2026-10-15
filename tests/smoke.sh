#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

CTL=${CTL:-"$ROOT/userspace/edu_ctl"}
LIFETIME=${LIFETIME:-"$ROOT/tests/edu_lifetime"}

if [ ! -x "$CTL" ]; then
    echo "FAIL: executable client missing: $CTL" >&2
    exit 1
fi

if [ ! -x "$LIFETIME" ]; then
    echo "FAIL: executable test client missing: $LIFETIME" >&2
    exit 1
fi

if [ ! -e /dev/edu0 ]; then
    echo "FAIL: /dev/edu0 missing; load and bind the driver first" >&2
    exit 1
fi

echo "== factorial =="

"$CTL" factorial 0
"$CTL" factorial 5
"$CTL" factorial 12

echo "== coherent DMA =="

for length in 1 63 64 65 255 256 257 4096; do
    "$CTL" dma-selftest "$length"
done

echo "== streaming DMA =="

for length in 1 63 64 65 255 256 257 4096; do
    "$CTL" dma-stream "$length"
done

echo "== repeated operations =="

"$LIFETIME" smoke dma
"$LIFETIME" smoke stream

echo "PASS: normal smoke commands completed successfully"
