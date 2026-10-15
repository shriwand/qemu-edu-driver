# Source Cross-Check of Limitations

## Audit scope

The cross-check uses the latest attached 32767-byte `edu_pci.c` snapshot reviewed during the streaming DMA lab. It predates the maintainer's subsequent fixes. Final build and test success are maintainer-reported. No final-source verification is claimed.

All five requested limitations are already supported by the available implementation. They are intentional properties or scope boundaries, not automatically defects to 'fix' by freeing resources earlier.

## 1. One op_lock serializes operations

Source evidence: `edu_ioctl()` takes `edev->op_lock` around `edu_run_factorial_locked()`, `edu_dma_selftest_locked()`, and `edu_dma_stream_selftest_locked()`. The streaming helper retains it through buffer preparation, DMA waits, drain, unmap, verification, and free. Cleanup also obtains this mutex before releasing hardware.

Conclusion: separate callers can queue concurrently, but hardware transactions are serialized per device. This lock does not serialize every software action: usercopy and diagnostic snapshot operations are outside the transaction lock.

## 2. Streaming timeout may return after its interval

Source evidence: `edu_dma_transfer_locked()` waits with `wait_for_completion_timeout()` and returns an error on expiry. Its streaming caller then executes `edu_dma_drain_locked()` before unmap and free.

Conclusion: the 1000 ms wait is not a syscall deadline. Mapping, mutex contention, test holds, scheduling, and drain can add latency. If hardware never becomes idle, the syscall might not return.

## 3. Permanently busy DMA can block drain

Source evidence: `edu_dma_drain_locked()` loops until the DMA command START bit clears, sleeping between polls, without a retry limit or timeout.

Conclusion: it can block a streaming operation or device cleanup indefinitely. This trades availability for the invariant that active DMA never targets unmapped/freed memory. No bounded abort/reset path is established.

## 4. QEMU unbind is not physical surprise removal

Source evidence: disconnect cleanup still reads MMIO while draining, acknowledging, and disabling sources. The code assumes the endpoint's control path remains accessible until the driver unmaps it.

Conclusion: managed unbind tests cover a different failure model than disappearance of a physical PCIe endpoint. The code does not supply a surprise-removal/AER protocol or prove that register reads after disappearance are safe or meaningful.

## 5. Driver-side hold does not prove hardware busy

Source evidence: the DMA helper writes the START command, drops state_lock, and then enters the optional test wait. The IRQ handler and device can progress during that wait.

Conclusion: the hold establishes an observable software point, not a frozen device. A test needs separate hardware-state evidence or a controlled device-model fault to claim busy-DMA unbind coverage.

## Historical discrepancies requiring final-revision confirmation

The attached snapshot still contained defects discussed and subsequently reported fixed:

- Two coherent allocations in probe, with the second overwriting ownership of the first.
- DMA completion suppression assigned and reset in the factorial path rather than the DMA path.
- MMIO helpers asserting op_lock even though probe held only state_lock.
- Independent admission `if` statements overwriting disconnect errors with fault errors.
- Missing validation for the newer test parameters.

These are not assertions that the maintainer's current code is broken. They establish why this snapshot cannot be certified as the final corrected revision. Confirm their absence against the final commit before publishing a code-audited release.

The snapshot contains no `dma_sync_single_for_cpu()` or `dma_sync_single_for_device()` reuse exercise. This is not an error in a valid one-shot map/unmap path; do not claim mapping-reuse support unless it exists in the final code and has been tested.
