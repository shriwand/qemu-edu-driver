#!/usr/bin/env bash
# smoke.sh - build + basic sanity without requiring QEMU guest.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "== layout =="
test -f "$ROOT/kernel/edu_pci.c"
test -f "$ROOT/kernel/edu_uapi.h"
test -f "$ROOT/kernel/Makefile"
test -f "$ROOT/userspace/Makefile"
echo "layout OK"

echo "== userspace build =="
make -C "$ROOT/userspace"
"$ROOT/userspace/edu_ctl" --help || true  # exits 0 on --help
echo "userspace build OK"

echo "== kernel build =="
if [ -d "/lib/modules/$(uname -r)/build" ]; then
  make -C "$ROOT/kernel"
  modinfo "$ROOT/kernel/edu_pci.ko" | grep -E "alias|description" || true
  echo "kernel build OK"
else
  echo "SKIP: no /lib/modules/$(uname -r)/build, install kernel headers (kernel-devel / linux-headers)"
  exit 1
fi

echo "== device presence (guest only) =="
if [ -e /dev/edu0 ]; then
  "$ROOT/userspace/edu_ctl" ident
  "$ROOT/userspace/edu_ctl" fact 5
  "$ROOT/userspace/edu_ctl" dma --len 256
else
  echo "SKIP device test: /dev/edu0 not present (load module in QEMU guest with -device edu)"
fi

echo "SMOKE OK"
