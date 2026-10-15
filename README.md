# qemu-edu-driver

A learning project in Linux PCI driver development: PCI resource management, MMIO, interrupts, a userspace ioctl interface, DMA, and safe software-object lifetime across controlled device unbind.

> Evidence status: the final project builds and all checks pass. The latest driver snapshot available for this documentation predates some reported fixes. This document describes the agreed corrected design; it is not an independent audit of an unavailable final revision. See `docs/code-audit.md` for the source-backed checks and historical discrepancies.

## Scope

The driver supports:

- Interrupt-driven factorial computation for inputs 0 through 12.
- One PCI interrupt vector, preferring MSI with an INTx fallback.
- A coherent DMA round-trip self-test.
- A one-shot streaming DMA round-trip self-test.
- Per-open contexts and reference-counted device state.
- Stale file descriptions that remain memory-safe after unbind.
- Probe, transaction, and completion fault-injection hooks.

This is an engineering learning project, not a claim of production readiness for arbitrary physical PCIe hardware.

## Device model

QEMU EDU has PCI ID `1234:11e8`, a 1 MiB MMIO BAR0, and a 4096-byte internal DMA buffer at device-local offset `0x40000`. Its default DMA address width is 28 bits. The driver must configure its DMA mask accordingly unless the device configuration and driver are deliberately changed together.

The DMA address returned by the Linux DMA API is not a CPU pointer. The internal offset `0x40000` is neither a CPU pointer nor the host DMA buffer address.

The QEMU device specification is available at https://www.qemu.org/docs/master/specs/edu.html.

## Layout

```
kernel/     edu_pci.c (driver), Makefile
include/    uapi/edu_uapi.h (ioctl ABI)
userspace/  edu_ctl.c (CLI), Makefile
tests/      smoke.sh, edu_lifetime.c, edu_abi_check.c, Makefile
docs/       design.md
```

## Prerequisites (host, once)

```bash
# Fedora:
dnf install -y qemu-system-x86 kernel-devel gcc make pciutils virtme-ng
# Debian/Ubuntu:
# apt install -y qemu-system-x86 linux-headers-$(uname -r) gcc make pciutils
# (virtme-ng: see https://github.com/arighi/virtme-ng — not in default Debian repos)
```

## Test bench (how to run the guest)

Option A — your current machine is the playground (easiest start):
nothing extra needed. `lspci -nn | grep 1234` will find no card — that is fine,
the first labs build without hardware.

Option B — a virtme-ng guest with the card (needed from LAB0 on — building
works without the card, but `probe()` only fires when the card is visible):

You need KVM access (`/dev/kvm`). First build that kernel with virtme-ng, then boot it
with the EDU card attached. virtme-ng boots your built kernel with the host
filesystem shared in, so the module you build here loads directly in the
guest — no disk image needed.

```bash
# for first time only
cd ../linux 
vng --build 
cd - 

# starting the VM
vng --run ../linux --disable-microvm --qemu-opts="-device edu" --user root --ssh --rwdir .

# connecting to the VM in 2 terminal
vng --ssh-client
```

What each step does:

- `cd ../linux && vng --build` — build the kernel in that tree
  (`vmlinux` + `arch/x86/boot/bzImage`). Re-run it after any kernel source
  change, then rebuild this module against that tree (from this directory,
  absolute path required — `make` resolves a relative `KDIR` from `kernel/`):
  `make -C kernel KDIR="$PWD/../linux"`.
- `vng --run ../linux` — boot the kernel built in that tree instead of the
  host kernel. Run it from this directory: it becomes the guest working
  directory too.
- `--disable-microvm` — use a full machine with a PCI bus. The default microvm
  machine has no PCI bus, and `edu` is a PCI device (`1234:11e8`).
- `--qemu-opts="-device edu"` — attach the EDU card. Keep the `=` form:
  a space-separated value is misparsed by vng's CLI.
- `--user root` — land as root, so `insmod`/`rmmod` work without sudo.
- `--ssh` — allows to connect using `vng --ssh-client` from another terminal. 
- `--rwdir .` — share state of current directory between host & vm. 


## Build & check (Inside guest)

```bash
make -C kernel && make -C userspace && make -C tests
lspci -nn | grep 1234   # expect 1234:11e8
insmod kernel/edu_pci.ko
bash tests/smoke.sh   # needs /dev/edu0: load and bind the driver in the guest first

dmesg | tail -20
```

One-shot (non-interactive) variant for quick checks:

```bash
vng --run ../linux --disable-microvm --qemu-opts="-device edu" --user root \
  --exec "lspci -nn | grep 1234"
```

Rebuild `edu_ctl`, `edu_lifetime`, and any ABI checker using the same checked-in UAPI header. Do not maintain independent copies of ioctl definitions.

An unbind/bind cycle does not replace loaded module code. After changing the driver binary, close outstanding test descriptors and reload the rebuilt module.

## Normal operations

Run these in the guest after loading and successfully binding the driver, with test hooks disabled:

```sh
userspace/edu_ctl factorial 5
userspace/edu_ctl dma-selftest 256
userspace/edu_ctl dma-stream 256
```

Factorial 5 must return 120. DMA commands return success only after the driver verifies the round-trip payload.

## Tests

The agreed test interface is:

```sh
./tests/edu_lifetime smoke dma
./tests/edu_lifetime smoke stream
./tests/edu_lifetime timeout dma
./tests/edu_lifetime timeout stream
./tests/edu_lifetime stale stream
./tests/edu_lifetime active stream
```

Timeout and active-operation tests require their specific module parameters and fresh bindings. They are not interchangeable with normal smoke tests. See `docs/testing.md`.

## Labs (as built)

This project was built in a different order than the classic
`probe → mmio → intx → msi → uapi → dma` tutorial. What follows
describes what the code actually does. There are no `v0-probe..v5-dma`
tags in this repo — the history is the commit messages
(`Lab 0`, `Lab 4` (factorial/UAPI), `Lab 2` (IRQ), `LAB3` (MSI),
`LAB 4` (coherent DMA), `LAB 4: Streaming DMA`).

Actual build order: PCI probe → UAPI + polled factorial → IRQ-driven
factorial → lifetime/teardown hardening → MSI fallback → coherent DMA
→ streaming DMA.

General rules: build the module against `../linux`
(`make -C kernel KDIR="$PWD/../linux"`), boot the guest with the command
from Option B, `insmod` there. One change — one `dmesg` check. Every
blocking wait has a timeout; fault latches (`faulted` / `dma_faulted`)
so a timed-out engine stays fenced until rebind.

### LAB0 — PCI binding and teardown

Objective: bind to `1234:11e8`, take ownership correctly, map BAR0, and
tear everything down cleanly on every error path and in `remove()`.

What the code does (not `pcim_*`/`devm_*`): plain
`pci_enable_device()` → `pci_request_region(pdev, 0, ...)` →
`pci_iomap(pdev, 0, 0)`, tracked by explicit ownership flags
(`pci_enabled`, `bar_requested`, `irq_requested`, `misc_registered`,
`irq_vectors_allocated`). All cleanup funnels through one
`edu_disconnect_and_cleanup()`: fence with `disconnected`, `complete_all`,
`misc_deregister`, `free_irq`/`pci_free_irq_vectors`, `dma_free_coherent`,
`pci_iounmap`, `pci_release_region`, `pci_disable_device` + `pci_clear_master`.
A `kref` keeps `struct edu_dev` alive while userspace fds are still open;
ioctls on a disconnected device return `-ENODEV`.

`probe()` also validates BAR0 (`IORESOURCE_MEM`, size ≥ `0x98 + 8`),
reads ident `0x00` with `ioread32`, clears stale IRQ state
(disable `0x80` + ack `0x24`→`0x64`), and refuses to bind with `-EBUSY`
if the factorial (`0x20` bit `0x01`) or DMA (`0x98` `START`) engine is busy.
`pci_set_master()` is done after DMA setup, just before IRQ setup.
There is no liveness `0x04` check and no probe-time self-test.

Failure injection for tests: `fail_step=1..10` module parameter.

### LAB1 — userspace ABI + factorial (polled first, IRQ later)

Objective: drive the card from userspace through `/dev/edu0` — without
`mmap`.

This came *before* the IRQ lab (commit `Lab 4`). First version was polled
(`readl_poll_timeout` on status `0x20`); the IRQ conversion happened in
the next lab. What remains today:

- `misc_register` (`/dev/edu0`, `MISC_DYNAMIC_MINOR`, `parent = &pdev->dev`)
  in `probe()`, `misc_deregister` in the common cleanup path.
  `unlocked_ioctl` + `compat_ioctl = compat_ptr_ioctl`.
- `include/uapi/edu_uapi.h`: only three ioctls, no `IDENT`, no `use_irq` flag:
  `EDU_IOCTL_FACTORIAL` (`_IOWR`, `0x1`, `{input, result, reserved[6]}`),
  `EDU_IOCTL_DMA_SELFTEST` (`_IOW`, `0x2`, `{length, reserved[3]}`),
  `EDU_IOCTL_DMA_STREAM_SELFTEST` (`_IOW`, `0x3`, same struct).
  Rule: `copy_from_user` → validate → act; every copy's return is checked,
  non-zero `reserved[]` is rejected with `-EINVAL`.
- Factorial validation: `input > 12` → `-ERANGE` (not `-EINVAL`).
  DMA validation: `length == 0 || length > 4096` → `-EINVAL`.
- `edu_ctl` subcommands: `factorial <0..12>`, `dma-selftest <1..4096>`
  (coherent), `dma-stream <1..4096>` (streaming). Non-zero exit with
  `perror`-style diagnostics on driver errors.

There is deliberately no `mmap` of BAR0: one ioctl choke point in the
kernel validates everything; raw MMIO never reaches userspace.

### LAB2 — IRQ (factorial goes from polled to interrupt-driven)

Objective: interrupt delivery with correct shared-line discipline, and a
single serialized operation model.

- `pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX)` in one
  call (see next lab for why combined); IRQ number from
  `pci_irq_vector(pdev, 0)`. `request_irq(..., IRQF_SHARED` only when
  INTx, `..., edev)` as dev_id. Freed via `free_irq` +
  `pci_free_irq_vectors` in the common cleanup — never managed.
- Handler: `status = ioread32(0x24)`; `0` → `IRQ_NONE`. Else ack with
  `iowrite32(status, 0x64)`, `atomic_inc(&irq_count)`, and `complete()`
  the matching completion (`factorial_done` for `0x01`,
  `dma_done` for `BIT(8)`). Unknown bits set the fault latches and wake
  waiters so they fail fast instead of hanging. There is no `0x60` raise
  self-test in `probe()` and no `/proc/interrupts`-counting lab step;
  the counter is exposed via debugfs `irq_count` instead.
- Factorial path today is IRQ-only (no polling fallback, no `--irq` flag):
  set status bit `0x80`, write `0x08`, `wait_for_completion_timeout`
  (1000 ms). Timeout → `-ETIMEDOUT` + fault latch + IRQ disable/ack;
  second call after a timeout → `-EIO`. Serialization via `op_lock`
  (mutex) + `state_lock` (spinlock); `-ERESTARTSYS` propagates from
  `mutex_lock_interruptible`.
- Concurrency/lifetime hardening (own lab step, `edu_lifetime.c`):
  `stale` (open fds survive unbind → `-ENODEV`), `active` (in-flight ioctl
  cancelled by disconnect → `-ENODEV`), `timeout` (first ioctl
  `-ETIMEDOUT`, second `-EIO` via `test_force_timeout` / `test_hold_ms` /
  `test_dma_hold_ms` / `test_publish_pause_ms` / `test_stream_fail_step=1..3`).
  `smoke` runs 100× iterations plus the 8-size DMA sweep.

### LAB3 — MSI with INTx fallback (one step, not two)

Objective: change the *delivery* mechanism, keep the device protocol.

Single `PCI_IRQ_MSI | PCI_IRQ_INTX` request; active mode is logged
(`mode=MSI/INTx, irq=…, vectors=1`) and detected via
`pci_dev_msi_enabled()` (`msi_enabled` flag). No handler change — it talks
to the device registers (`0x24`/`0x64`), not to the line, so the `0x64` ack
is still mandatory under MSI. Verify with `lspci -vv` (`MSI Enable+` vs
`INTx+`) and the `/proc/interrupts` line type (`PCI-MSI` vs `IO-APIC`).

Deviations from the tutorial text: INTx and MSI were never separate tagged
labs here; there is exactly one IRQ setup path with automatic fallback.

### LAB4 — DMA: coherent first, then streaming (both IRQ-driven)

Objective: move buffers without ever handing a userspace pointer to the
device. Two commits: `LAB 4 (Coherent DMA)` then `LAB 4: Streaming DMA`.

- Mask first in `probe()`: `dma_set_mask_and_coherent(&pdev->dev,
  DMA_BIT_MASK(28))`. Probe fails if the 28-bit window is unavailable.
- Coherent: one `dma_alloc_coherent(8192)` in `probe()` (two 4096 halves:
  TX at offset 0, RX at offset 4096), `dma_free_coherent` in cleanup.
  Self-test: pattern-fill TX → `RAM→EDU` to device address `0x40000` →
  `EDU→RAM` into RX → `memcmp` + RX-tail guard check. Exposed as
  `EDU_IOCTL_DMA_SELFTEST` (`length` in, no `passed` out — `0` means pass).
- Streaming: same transfer via `dma_map_single` / `dma_unmap_single` on
  `kmalloc(4096)` TX/RX buffers, with `dma_mapping_error` checks and a
  `dma_drain_locked` (poll `START` clear, `usleep_range`) after each leg so
  unmap never races the engine. Exposed as
  `EDU_IOCTL_DMA_STREAM_SELFTEST`. Buffer ownership rule: do not touch the
  buffer between map and unmap. `get_user_pages` zero-copy is explicitly
  out of scope — `edu_ctl dma-*` passes only a length.
- Both paths are IRQ-driven (`START | IRQ_ENABLE [+ FROM_DEVICE]` in `0x98`,
  `wait_for_completion_timeout` 1000 ms on `dma_done`), programmed with
  `writeq`/`readq` for `0x80/0x88/0x90/0x98` (not lo/hi 32-bit pairs).
  `dmesg` shows the coherent base below `0x10000000` (28-bit window).

Done when: `edu_ctl dma-selftest 256`/`4096` and `edu_ctl dma-stream
256`/`4096` PASS; `tests/smoke.sh` (8 sizes × both paths + 100×
`edu_lifetime smoke dma/stream`) prints `PASS`; `tests/edu_abi_check`
prints the three ioctl encodings for ABI review.

Watch for: 64-bit registers need full 64-bit accesses here; small lengths
(1, 63, 64, 65, …) catch off-by-ones — that is why the sweep is in
`smoke.sh`.

## Debug cheat sheet

```bash
make -C kernel && make -C userspace && make -C tests
insmod kernel/edu_pci.ko; dmesg | tail -20
lspci -nn | grep 1234; lspci -vv -s 00:05.0  # your BDF here!
cat /proc/interrupts | grep edu
ls /sys/class/misc/  # edu0 is the misc device
rmmod edu_pci; dmesg -C
```

Rule: one step — one check in `dmesg`. A hang means a missing timeout; fix it with a timeout.
