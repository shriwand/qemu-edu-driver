# Test Plan and Evidence Boundaries

## Current evidence status

The maintainer reports that the final project builds and all checks pass. The final source revision and raw test logs were not attached. This is an author-reported validation result, not an independently reproduced test report. No timings, sanitizer coverage, or guest configurations are invented below.

Before a published release, preserve the commit ID, guest kernel/configuration, QEMU and virtme-ng versions, compiler version, launch command, and actual outputs with the run.

## Preconditions

- Run hardware tests in the guest, not against a host PCI device.
- Load the rebuilt module and confirm successful binding, not just successful insmod.
- Rebuild both clients using the same UAPI header.
- Close outstanding misc/debugfs descriptors before ordinary module unload.
- Use a fresh binding for each independent timeout test.
- Discover the actual BDF with `lspci -nn -d 1234:11e8`.
- Keep fault hooks disabled for normal tests.

## Normal tests

```sh
userspace/edu_ctl factorial 0
userspace/edu_ctl factorial 5
userspace/edu_ctl factorial 12
userspace/edu_ctl dma-selftest 256
userspace/edu_ctl dma-stream 256
./tests/edu_lifetime smoke dma
./tests/edu_lifetime smoke stream
```

Expected factorial results are 1, 120, and 479001600. Both DMA clients must run actual round trips and verify payload, not merely return after an IRQ.

Boundary DMA sizes: 1, 63, 64, 65, 255, 256, 257, 4096. Invalid requests: 0, 4097, and nonzero reserved fields. If the CLI rejects invalid values locally, use a dedicated raw ioctl request to test the kernel validation branch.

## Timeout tests

On a fresh binding loaded with `test_force_dma_timeout=1`:

```sh
./tests/edu_lifetime timeout dma
```

Repeat on another fresh binding:

```sh
./tests/edu_lifetime timeout stream
```

The first request must return ETIMEDOUT; the second must return EIO. Check that log lines identify `dma` or `stream`, not factorial. For stream, mandatory drain and unmap/free occur before the operation releases op_lock.

Verify the parameter through sysfs. The hook must suppress DMA software notification while retaining device ACK. It does not emulate a permanently stuck DMA engine.

Factorial timeout uses its separate `test_force_timeout` hook and `timeout factorial` mode.

## Stale descriptors

Run `./tests/edu_lifetime stale stream`, unbind in another terminal using the actual BDF, then continue the client. Repeat for coherent DMA.

Expected: old supported ioctls return ENODEV; unbind does not wait for all fd closes; the software object survives until the final open file description is released. Three opens create three driver file references; dup creates another descriptor for an existing file description, not a fourth driver open reference.

## Active operations

Load with `test_dma_hold_ms=10000`, start `active stream`, wait for the logged start point, and unbind in another terminal. Test factorial similarly with `test_hold_ms`.

Expected: software waiter wakes and returns ENODEV, resource cleanup is ordered, and the fd can remain open safely. This establishes software cancellation, not necessarily hardware-busy removal: the device may finish before or during the driver hold.

## Partial probe and stream failures

Probe injection after coherent allocation is step 10. Verify failed binding, allocation cleanup, and successful subsequent loading without injection. An open failure is expected when misc registration was never reached; do not expect a DMA timeout from an unpublished device.

Probe steps 8/9 exercise vector ownership before handler registration; step 5 exercises cleanup after handler registration; step 7 requires the publication window to test an already-open fd after failed probe.

Streaming failure steps:

| Step | Boundary | Expected cleanup |
|---|---|---|
| 1 | After TX map, before TX start | TX unmap; both buffers freed |
| 2 | TX complete/unmapped, before RX map | Both buffers freed |
| 3 | RX mapped, before RX start | RX unmap; both buffers freed |

These hooks simulate control-flow failures, not internal failures of the DMA subsystem itself.

## Interrupt modes

Verify MSI Enable and BusMaster in lspci and the allocated handler in /proc/interrupts. Use a separately configured guest with `pci=nomsi` to test INTx fallback. Do not pass this parameter to the host accidentally.

The same operation, ACK, timeout, and lifetime invariants must hold in both modes. Linux MSI guidance is available at https://docs.kernel.org/PCI/msi-howto.html.

## Diagnostics

- Compiler warnings and checkpatch provide limited static feedback.
- Lockdep checks runtime locking assumptions when enabled.
- KASAN detects classes of CPU memory errors; do not treat its silence as proof that device DMA cannot corrupt memory.
- DMA API debugging can detect mapping API misuse when configured and active.
- Allocation/free instrumentation is needed to investigate duplicate coherent allocation leaks; a successful subsequent probe alone is not proof of leak freedom.

Record which tools were enabled. Do not mark unavailable tools as passing.

## Suggested result record

For each scenario record: commit, environment, loaded module parameters, command, expected errno/result, observed errno/result, kernel log, PASS/FAIL, and the limit of the conclusion.

Missing evidence is 'not recorded', not PASS. A test binary's operation label and selected ioctl must agree before its output is used as driver evidence.
