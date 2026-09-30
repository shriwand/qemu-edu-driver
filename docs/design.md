# design.md — your notebook, fill it in yourself

This file is not a reference, it is a worksheet. Write down what you learn.

## 1. BAR0 map (fill in from qemu docs/specs/edu.rst)

| Offset | Access | What is there | How I will verify it |
|---|---|---|---|
| 0x00 | | | |
| 0x04 | | | |
| 0x08 | | | |
| 0x20 | | | |
| 0x24 | | | |
| 0x60 | | | |
| 0x64 | | | |
| 0x80–0x98 | | | |
| 0x40000 | | | |

Questions to yourself:
- How many bits is the default DMA window? What is `dma_mask`?
- INTx vs MSI — what is the difference? Why is the 0x64 ack needed in both cases?
- Which access sizes are allowed below/above 0x80 (size 4 vs 8)?

## 2. Architecture (sketch after LAB2)

- Where do you store `mmio`? (Did you design `struct edu_dev` yet?)
- Who owns resources: `pcim_*` vs `pci_*`? Where is cleanup?
- How does the ISR know the interrupt is ours? (`IRQ_NONE` vs `IRQ_HANDLED`)

## 3. ABI (after LAB5)

- Which ioctls do you really need? Numbers, structs, who comes first — kernel or client?
- How to version it when it changes?

## 4. Test plan (check off)

- [ ] `lspci -nn | grep 1234` sees the card
- [ ] `insmod` + `dmesg` with no warnings
- [ ] liveness `~` check passes
- [ ] `5! == 120` through the driver
- [ ] `raise` makes the IRQ counter grow
- [ ] DMA round-trip PASS at 256 and 4096 bytes
