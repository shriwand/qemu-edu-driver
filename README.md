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
sudo dnf install -y qemu-system-x86 kernel-devel gcc make pciutils virtme-ng
# Debian/Ubuntu:
# sudo apt install -y qemu-system-x86 linux-headers-$(uname -r) gcc make pciutils
# (virtme-ng: see https://github.com/arighi/virtme-ng — not in default Debian repos)

bash tests/smoke.sh   # must build: edu_pci.ko + edu_ctl
```

## Test bench (how to run the guest)

Option A — your current machine is the playground (easiest start):
nothing extra needed. `lspci -nn | grep 1234` will find no card — that is fine,
the first labs build without hardware.

Option B — a virtme-ng guest with the card (needed from LAB2 on):

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

### LAB1 — probe/bind
1. `make -C kernel && sudo insmod kernel/edu_pci.ko
2. `dmesg | tail` — do you see `probe hit`? No card yet — fine, probe will not fire.
3. Implement in `edu_probe`: `pcim_enable_device()`, `pci_set_master()`,
   `pcim_request_region(BAR0)` + `pcim_iomap()`, print BAR length via `dev_info`.
4. Debug: `lspci -vv -s <bdf>`, `cat /proc/iomem | grep edu`, `dmesg`.
   Unload: `sudo rmmod edu_pci`.

### LAB2 — MMIO
1. Define registers `0x00/0x04/0x08/0x20` in `edu_pci.c`.
2. Read magic `0x00`, check liveness: write `0x12340000` to `0x04`,
   read back — expect `~` (bitwise inversion).
3. Factorial: write N to `0x08`, poll for bit `0x01` in `0x20` to clear
   (poll with timeout!), read back the result. Check `5! == 120`.
4. Debug: `dev_info` after every step; on a hang — add a timeout returning
   `-ETIMEDOUT`, never a bare infinite `while`.

### LAB3 — IRQ
1. `pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX)`,
   `request_irq(pci_irq_vector(...))`.
2. ISR: read `0x24`; if 0 return `IRQ_NONE`; else write it to `0x64`,
   bump a counter, `wake_up`. Trigger by writing to `0x60`.
3. Debug: `cat /proc/interrupts | grep edu`, `lspci -vv | grep -i msi`.

### LAB4 — DMA
1. `dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28))` — figure out why 28.
2. `dma_alloc_coherent()` + engine regs `0x80/0x88/0x90/0x98`:
   RAM→EDU `@0x40000`, then back, `memcmp`. Length ≤ 4096.
3. Wait for the `START` bit to clear, with timeout. Debug: `dmesg`, byte-wise compare.

### LAB5 — userspace (only after LAB2–4 via sysfs/debugfs)
1. Design 2–3 ioctls in `edu_uapi.h` (IDENT, FACTORIAL, DMA_TEST).
2. Add `miscdevice` + `unlocked_ioctl` to the driver.
3. Write `edu_ctl.c`: `open("/dev/edu0")` + `ioctl()` + printing.

## Debug cheat sheet

```bash
make -C kernel && sudo insmod kernel/edu_pci.ko; dmesg | tail -20
lspci -nn | grep 1234; lspci -vv -s 00:05.0  # your BDF here!
cat /proc/interrupts | grep edu
ls /sys/class/misc/  # edu0 appears only after LAB5 (misc)
sudo rmmod edu_pci; sudo dmesg -C
```

Rule: one step — one check in `dmesg`. A hang means a missing timeout; fix it with a timeout.
