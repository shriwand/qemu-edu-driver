# qemu-edu-driver

Minimal PCI driver for the **QEMU EDU** educational device (`1234:11e8`),
plus a tiny userspace CLI. Covers MMIO, IRQ (INTx/MSI) and DMA.

## Layout

```
kernel/     edu_pci.c (driver), edu_uapi.h (ioctl ABI), Makefile
userspace/  edu_ctl.c (CLI), Makefile
tests/      smoke.sh
docs/       design.md
```

## Prerequisites (host, once)

```bash
# Fedora:
dnf install -y qemu-system-x86 kernel-devel gcc make pciutils virtme-ng
# Debian/Ubuntu:
# apt install -y qemu-system-x86 linux-headers-$(uname -r) gcc make pciutils
# (virtme-ng: see https://github.com/arighi/virtme-ng — not in default Debian repos)

bash tests/smoke.sh   # must build: edu_pci.ko + edu_ctl
```

## Test bench (how to run the guest)

Option A — your current machine is the playground (easiest start):
nothing extra needed. `lspci -nn | grep 1234` will find no card — that is fine,
the first labs build without hardware.

Option B — a virtme-ng guest with the card (needed from LAB0 on — building
works without the card, but `probe()` only fires when the card is visible):

You need a kernel tree in `../linux` (a sibling of this repo) and KVM
access (`/dev/kvm`). First build that kernel with virtme-ng, then boot it
with the EDU card attached. virtme-ng boots your built kernel with the host
filesystem shared in, so the module you build here loads directly in the
guest — no disk image needed.

```bash
# for first time only
cd ../linux 
vng --build 
cd - 

vng --run ../linux --disable-microvm --qemu-opts="-device edu" --user root --ssh
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

Inside the guest:

```bash
lspci -nn | grep 1234   # expect 1234:11e8
insmod kernel/edu_pci.ko
dmesg | tail -20
```

One-shot (non-interactive) variant for quick checks:

```bash
vng --run ../linux --disable-microvm --qemu-opts="-device edu" --user root \
  --exec "lspci -nn | grep 1234"
```

## Labs

## Labs

Work lab by lab. Each lab ends with a commit and a tag, so the design
evolution stays visible (`git log --oneline v0-probe..v5-dma` is your
interview story). If a lab goes sideways, `git stash` plus
`git checkout vX` gives you the last known-good stage to restart from.

Tag map: `v0-probe → v1-mmio → v2-intx → v3-msi → v4-uapi → v5-dma`.

General rules for all labs: build the module against `../linux`
(`make -C kernel KDIR="$PWD/../linux"`), boot the guest with the command
from Option B, `insmod` there. One change — one `dmesg` check. A hang always
means a missing timeout; never ship a bare infinite poll.

### LAB0 — PCI binding (`v0-probe`)

Objective: bind to `1234:11e8`, take ownership correctly, map BAR0.

Background: the PCI core matches your `pci_device_id` table against every
device and calls `probe()` on a match. Returning 0 means "this device is
mine now". Everything acquired in `probe()` must be released on every error
path and in `remove()` — except resources requested through the managed
(`pcim_*` / `devm_*`) API, which the core unwinds automatically, in reverse
order, on probe failure or `remove()`. Knowing for each acquisition which
category it belongs to is the main skill of this lab.

Steps:
1. Declare `pci_device_id` (`PCI_DEVICE(0x1234, 0x11e8)`), `pci_driver`
   with `probe`/`remove`, register with `module_pci_driver`. Build, boot
   the guest, `insmod`. Confirm binding: `lspci -k -s <bdf>` shows
   "Kernel driver in use: edu" and `dmesg` shows your probe message.
2. In `probe()`: `pcim_enable_device()` → `pci_set_master()` →
   `pcim_request_region(pdev, 0, "edu_bar0")` → `pcim_iomap(pdev, 0, 0)`.
   Allocate a per-device struct (`struct edu_dev` via `devm_kzalloc`),
   store the `__iomem` pointer there, `pci_set_drvdata(pdev, e)`. Log the
   BAR length (`pci_resource_len(pdev, 0)` — expect 1 MiB) and flags.
3. `remove()` logs only — the three acquisitions above are managed, the
   core releases them. Prove it: a `rmmod`/`insmod` cycle is clean, with no
   "unable to reserve" errors in `dmesg`.
4. Write error paths with `goto` labels from the start, even though every
   call here is managed — LAB2 adds a non-managed allocation and you will
   slot its cleanup into the same structure.

Done when: bind/unbind cycles are clean; `cat /proc/iomem | grep edu`
shows the BAR; `modinfo` lists the `pci:v00001234d000011E8` alias.
Then commit and `git tag v0-probe`.

Watch for: `pcim_iomap` returning NULL (BAR not reserved first); `-EBUSY`
from `pcim_request_region` means something else owns the BAR — usually a
stale copy of your own driver still loaded (`rmmod` it).

### LAB1 — MMIO: ident, liveness, factorial (`v1-mmio`)

Objective: first real conversation with hardware — all polled, no IRQ yet.

Background: EDU lives in BAR0 MMIO; the register reference is QEMU's
`docs/specs/edu.rst`. Record the map in `docs/design.md` as you go — that
table is yours to fill. Access-size rule: 4-byte accesses below `0x80`,
4- or 8-byte above; this driver uses 32-bit accesses everywhere (the u64
DMA registers as lo/hi pairs — legal and portable).

Steps:
1. `#define` the registers you touch (`0x00` ident, `0x04` liveness,
   `0x08` factorial, `0x20` status with bit `0x01` busy). Read ident with
   `ioread32` in `probe()`, parse major/minor, `dev_info` it.
2. Liveness: write `0x12340000` to `0x04`, read back, require the bitwise
   inverse (`~`). A mismatch is `-ENODEV`-worthy in a real driver; here a
   `dev_warn` plus continue is acceptable — but write down why you chose it.
3. Factorial: write `0` to status `0x20` (clears "irq after factorial"),
   write N to `0x08`, poll status bit `0x01` until clear with a bounded
   wait (`jiffies` + `msecs_to_jiffies`, `udelay`/`cpu_relax` in the loop,
   `-ETIMEDOUT` on expiry). Read the result back from `0x08`.
4. Validate input: `N > 12` overflows u32 (`13!` does not fit) → reject with
   `-EINVAL` before touching hardware. Put this in a helper
   (`edu_factorial_sync(e, n, &res)`) — LAB4 will call it from ioctl.

Done when: a probe-time self-test computes `5! = 120` (log it), `N = 13`
is rejected, and the timeout path is at least code-reviewed (forcing a real
timeout needs fault injection — note it as known-uncovered).
Then commit and `git tag v1-mmio`.

Watch for: polling without a timeout (a guest freeze on the first emulator
quirk); reaching for `memcpy_toio` where the `ioread32`/`iowrite32` family
is the right tool.

### LAB2 — legacy INTx (`v2-intx`)

Objective: interrupt delivery over the classic PCI line, with correct
shared-line discipline.

Background: INTx lines can be shared between devices. Your handler also runs
for *other* devices' interrupts — so its first job is asking the *device*
(not the kernel) "was that you?". EDU answers in register `0x24` (0 means
not us → return `IRQ_NONE`). Acknowledging is a *device* action (write the
status bits to `0x64`); without it the line stays asserted.

Steps:
1. `pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_INTX)`; the Linux IRQ number
   is `pci_irq_vector(pdev, 0)` — never use `pdev->irq` directly with this
   API. `request_irq(..., handler, IRQF_SHARED, "edu", e)` with your struct
   as dev_id. This allocation is NOT managed: `pci_free_irq_vectors()` goes
   in `remove()` and on every later error path (extend the LAB0 `goto`
   chain).
2. Handler: `status = ioread32(0x24)`; if 0 → `IRQ_NONE`. Else
   `iowrite32(status, 0x64)`, `atomic_inc(&e->irq_count)`,
   `wake_up_interruptible(&e->wq)`, return `IRQ_HANDLED`.
3. Self-test in `probe()` (no userspace yet): snapshot the counter,
   `iowrite32(0x1, 0x60)` (raise), `wait_event_interruptible_timeout` for
   the counter to change (1 s). On failure `dev_err` — decide whether to
   fail the probe, and document why.
4. Fire several raises in a row; the counter must equal the number fired.

Done when: the counter tracks raises 1:1; `/proc/interrupts` shows your
line counting up; `rmmod` with a pending raise does not hang.
Then commit and `git tag v2-intx`.

Watch for: returning `IRQ_HANDLED` unconditionally (breaks line sharing and
hides other devices' interrupts); forgetting the ack (interrupt storm —
`dmesg` flood and a locked guest; the ack write is the fix).

### LAB3 — MSI (`v3-msi`)

Objective: change the *delivery* mechanism and learn what stays the same.

Background: MSI replaces the physical line with a device-issued memory
write — no sharing, no line routing. Detecting and acknowledging *at the
device* is unchanged: EDU still reports in `0x24` and still requires the
`0x64` ack, otherwise it keeps interrupting. Mental model with two levels:
controller-level (line vs message) and device-level (status/ack registers)
— MSI changes only the first.

Steps:
1. Request `PCI_IRQ_MSI` first; if the platform denies it, fall back to
   `PCI_IRQ_INTX` and `dev_info` which mode won (one flag expression,
   `PCI_IRQ_MSI | PCI_IRQ_INTX`, then inspect what you got). No handler
   change is needed — be able to say out loud why (the handler talks to
   the device, not to the line).
2. Verify the mode: `lspci -vv -s <bdf>` shows `MSI Enable+` (previously
   `INTx+`); the `/proc/interrupts` line type changes to `PCI-MSI`.
3. Re-run the LAB2 self-test unchanged — the same 1:1 counting must hold.

Done when: MSI is the active mode on this QEMU machine, the self-test
passes unmodified, and you can explain why the `0x64` write survived the
switch. Then commit and `git tag v3-msi`.

Watch for: assuming MSI needs no ack (the storm returns); hard-coding MSI
without fallback (breaks on machines without MSI support).

### LAB4 — minimal userspace ABI (`v4-uapi`)

Objective: drive the card from userspace through `/dev/edu0` — without
`mmap`.

Responsibility split (the contract — also record it in `docs/design.md`):

| Userspace (`edu_ctl`) | Driver |
|---|---|
| builds the request, prints the result | validates every argument first |
| knows nothing about registers | programs MMIO, never trusts the caller |
| blocks in `ioctl`, handles errors | waits for IRQ with timeout, owns DMA mapping |

Background: `mmap`ing BAR0 into userspace would hand raw hardware to an
unprivileged process (wrong caching → silent corruption, zero validation).
`ioctl` keeps a single validation choke point in the kernel — right for v1.

Steps:
1. `misc_register` (`/dev/edu0`, `MISC_DYNAMIC_MINOR`) in `probe()`,
   `misc_deregister` in `remove()` (not managed — extend the cleanup).
   `unlocked_ioctl` plus `compat_ioctl` pointing at the same function (no
   pointer-carrying structs yet, so compat is trivially safe).
2. In `edu_uapi.h`: `EDU_IOCTL_IDENT` (`_IOR`, returns magic/major/minor)
   and `EDU_IOCTL_RUN_FACTORIAL` (`_IOWR`, in: `n`, `use_irq`; out:
   `result`). The rule: `copy_from_user` → validate (`n ≤ 12`, else
   `-EINVAL`) → act → `copy_to_user`; check every copy's return value.
3. Factorial path: reuse the LAB1 helper for polling; if `use_irq`, set
   status bit `0x80` first and `wait_event_interruptible_timeout` on the
   LAB2 waitqueue instead of polling. Return `-ETIMEDOUT` on expiry and let
   `-ERESTARTSYS` propagate (a signal interrupted the wait — correct).
4. `edu_ctl.c`: `ident` and `fact <n> [--irq]` subcommands, non-zero exit
   on driver errors, `perror`-style diagnostics.

Done when: `edu_ctl ident` prints the magic; `edu_ctl fact 5` → 120;
`edu_ctl fact 13` → a clean `EINVAL` (not a crash, not a hang); `--irq`
and polling agree. Then commit and `git tag v4-uapi`.

Watch for: trusting userspace values (validate everything after copying);
`copy_*_user` in atomic or IRQ context (never — ioctls run in process
context, keep it that way).

### LAB5 — DMA (`v5-dma`)

Objective: move buffers — coherent first, then streaming — and learn why a
userspace pointer must never reach the device.

Background: EDU's DMA window is 28 bits by default (the QEMU `dma_mask`
property) — it can only address the first 256 MiB. The driver must program
a mask the device can actually drive, and every DMA address handed to the
card must come from the DMA API. A process virtual address is meaningless
to hardware (unmapped after swap, wrong after `fork`, reachable from
userspace).

Steps:
1. Mask first, in `probe()`: `dma_set_mask_and_coherent(&pdev->dev,
   DMA_BIT_MASK(28))`. Fail the probe if it fails — the device promises 28
   bits, so failure means a broken environment, and a silent fallback would
   hide it. Log the working mask.
2. Coherent (first commit): `dma_alloc_coherent(4096)` in `probe()` — the
   simplest lifetime; store the CPU and DMA addresses in `struct edu_dev`,
   `dma_free_coherent` in `remove()`. Round-trip self-test: fill a pattern
   → program `0x80/0x88/0x90/0x98` as 32-bit lo/hi pairs (the LAB1
   access-size rule) → `START`, poll for clear with timeout → reverse
   direction (bit `0x02`) → `memcmp`. The EDU-side address is `0x40000`,
   length ≤ 4096. Expose as `EDU_IOCTL_DMA_TEST` (`len` in, `passed` out).
3. Streaming (second commit): the same transfer via `dma_map_single()` /
   `dma_unmap_single()` on a normal `kmalloc` buffer; check
   `dma_mapping_error()`; do not touch the buffer between map and unmap —
   ownership belongs to the device there (cache coherency in one sentence:
   the CPU cache may hold stale lines; the API handles it, but only if you
   stay away mid-transfer).
4. Userspace-pointer rule: `edu_ctl dma` passes only a length; the driver
   owns all buffers. Pinning user pages (`get_user_pages` + map) exists for
   zero-copy futures — explicitly out of scope for v1; name it in
   `design.md` so an interviewer sees you know the road not taken.

Done when: `edu_ctl dma --len 256` and `--len 4096` PASS on both paths;
`dmesg` shows DMA addresses below `0x10000000` (inside the 28-bit window);
`rmmod` frees without leaks. Then commit and `git tag v5-dma` — and
`git log --oneline v0-probe..v5-dma` is the story you tell on interviews.

Watch for: writing the 64-bit DMA registers with one 64-bit store
(violates the size rule — use lo/hi); `dma_alloc_coherent` without a
matching free on the error path; testing only length 4096 (small lengths
catch off-by-ones).

## Debug cheat sheet

```bash
make -C kernel && insmod kernel/edu_pci.ko; dmesg | tail -20
lspci -nn | grep 1234; lspci -vv -s 00:05.0  # your BDF here!
cat /proc/interrupts | grep edu
ls /sys/class/misc/  # edu0 appears only after LAB4 (misc)
rmmod edu_pci; dmesg -C
```

Rule: one step — one check in `dmesg`. A hang means a missing timeout; fix it with a timeout.
