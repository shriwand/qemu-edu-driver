# Lifetime, Locking, and Teardown

> Evidence status: the maintainer reports that the final project builds and all checks pass. The latest driver snapshot available for this documentation predates some reported fixes. This document describes the agreed corrected design; it is not an independent audit of an unavailable final revision. See `docs/code-audit.md` for the source-backed checks and historical discrepancies.

## Two lifetimes

Hardware lifetime includes MMIO, IRQ registration, vector allocation, PCI state, coherent allocations, and streaming mappings. Software-object lifetime includes `struct edu_dev` and the state required to reject requests from stale file descriptions.

A valid `edu_dev *` is not permission to access hardware. Serialization is not lifetime protection.

## Logical states

```text
initialization -> live -> disconnecting -> detached
        |                  ^
        +-- probe failure -+
```

The implementation uses `disconnected` rather than requiring this diagram to be a literal enum. Factorial and DMA fault flags are separate operation-health states. A faulted engine can remain attached; a disconnected device admits no new hardware operation.

For a supported ioctl that reaches the hardware admission check, disconnect takes precedence over an engine fault. Early command and user-memory validation can have their own error precedence. Do not promise that every malformed request on a stale fd returns ENODEV.

## Reference ownership

| Owner | Acquisition | Release |
|---|---|---|
| PCI driver binding | `kref_init()` during allocation | Base `kref_put()` after teardown |
| Each successful open file description | `kref_get()` in `.open()` | `kref_put()` in `.release()` |
| Per-open context | `kzalloc()` in `.open()` | `kfree()` in `.release()` |

`dup()` does not invoke the driver's `.open()` again. Duplicated descriptors refer to the same open file description, whose final release returns its one driver reference. Inherited file descriptions across fork follow the same lifetime principle.

The shared object uses `kzalloc()`, not device-managed allocation: it may outlive the PCI binding. Its final kref callback is software-only and must not access MMIO, IRQ state, or an unretained PCI device pointer. A copied BDF string can be used for late diagnostics.

The initial pointer lookup in misc `.open()` also depends on the misc-core open/deregister synchronization protocol. A kref cannot rescue a lookup that already points into freed memory.

## Locks and notifications

| Mechanism | Responsibility |
|---|---|
| `op_lock` mutex | Serialize complete hardware transactions; exclude teardown from their resource-use paths |
| `state_lock` spinlock | Protect short state transitions and MMIO sections shared with the IRQ handler |
| `kref` | Keep the software object allocated |
| Completions | Notify waiters; not proof of result validity or DMA idle |
| Device busy/start bits | Device-specific evidence of hardware progress/idle |

The normal nested order is `op_lock -> state_lock`. Disconnect acquires `state_lock` to fence and wake, releases it, and only then waits for `op_lock`.

Never call `free_irq()` while holding a spinlock that the handler needs. Never sleep while holding `state_lock`.

## Factorial operation

```text
copy input from userspace
-> acquire op_lock
-> check disconnect/fault under state_lock
-> clear stale state, reinitialize completion, start hardware
-> release state_lock, keep op_lock
-> wait for completion
-> reacquire state_lock
-> check disconnect before result MMIO
-> check timeout/fault/busy and capture result
-> release locks
-> copy software result to userspace
```

If the result was accepted before disconnect, a later software-only copyout may complete successfully after disconnect. EFAULT during copyout does not mean the hardware operation never happened.

## Coherent DMA

The corrected setup creates one 8192-byte coherent allocation. TX occupies the first 4096 bytes, RX the second 4096 bytes. The self-test copies TX to the internal EDU buffer and then copies the internal buffer to RX.

CPU accesses are ordered according to the transaction protocol. Coherency removes explicit per-transfer cache maintenance; it does not remove barriers, races, or lifetime obligations.

On DMA timeout, the fault latch blocks subsequent DMA starts. The coherent allocation remains allocated until hardware drain and device teardown; it is not freed by the failing ioctl.

## Streaming DMA

```text
acquire op_lock
-> allocate separate TX/RX kernel buffers
-> fill TX before map
-> map TX as DMA_TO_DEVICE
-> transfer RAM to EDU
-> drain hardware even if the transfer helper returned an error
-> unmap TX
-> map RX as DMA_FROM_DEVICE
-> transfer EDU to RAM
-> drain hardware
-> unmap RX
-> verify payload while CPU owns buffers
-> free buffers
-> release op_lock
```

Mapping failures are checked with `dma_mapping_error()`. Unmap uses the original device, DMA address, mapping size, and direction. A successful kmalloc is not a successful DMA mapping.

Only transferred RX bytes are verified in the one-shot streaming path. The unchanged-tail test used for coherent memory is not assumed portable for a larger DMA_FROM_DEVICE mapping with only a partial hardware write, particularly with bounce buffering.

## Timeout latency

The completion wait interval is 1000 ms in the reviewed snapshot. It is not an upper bound on total syscall duration. Time can also be spent waiting for `op_lock`, in test holds, in scheduling, and in mandatory streaming hardware drain.

ETIMEDOUT indicates failure to receive the expected notification within that wait interval. It does not cancel the hardware transfer. A permanently busy engine can prevent the ioctl from returning at all because releasing a still-active mapping is unsafe.

## Disconnect and teardown

The corrected design follows these phases:

1. Set `disconnected` under `state_lock`.
2. Wake factorial and DMA waiters and the test wait queue.
3. Deregister the miscdevice and remove debugfs entries.
4. Acquire `op_lock` to drain operation resource use, not to wait for fd closure.
5. Wait for hardware DMA START to clear while MMIO remains valid.
6. Disable the factorial interrupt source and acknowledge pending causes.
7. Call `free_irq()` without `state_lock` held.
8. Free the PCI IRQ vectors.
9. Free the coherent allocation after DMA idle.
10. Unmap MMIO, clear bus mastering/disable the function, and release BAR ownership in the agreed cleanup order.
11. Clear drvdata and drop the binding's base reference.

Streaming mappings belong to the active operation and are gone before teardown obtains `op_lock`. Stale files retain only software-object references.

The IRQ handler may still acknowledge causes after the software fence, while handler resources and MMIO are valid. The fence blocks new user operations; it does not instantly destroy the IRQ transport. Internal drain may read control registers after disconnect because it is part of resource-safe teardown, not a new user transaction.

## Publication and probe failure

Before misc publication, no misc fd exists. After successful `misc_register()`, an external file reference may already exist even if probe later fails. Post-publication unwind therefore requires fencing, deregistration, transaction drain, hardware cleanup, and a base kref put. Direct kfree is unsafe in that phase.

Resource flags represent successful acquisitions, not merely attempted calls. An unsuccessful PCI enable must not enter a cleanup path that treats enable as acquired.

## Diagnostics

Debugfs captures `irq_count` into an independent snapshot during open. Later read/release use that snapshot rather than `edu_dev`. Removal must synchronize with the protected debugfs callback before the device object is freed. A read after removal may be rejected by the debugfs proxy; success after removal is not guaranteed.

## Model boundaries

This design assumes controlled QEMU unbind and readable device registers during drain. It does not establish a safe polling protocol after a physical endpoint has disappeared, nor does it implement AER recovery or a hardware abort/reset mechanism.

The DMA API ownership rules are documented at https://docs.kernel.org/core-api/dma-api-howto.html.
