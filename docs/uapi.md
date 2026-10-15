# Userspace ABI

> Evidence status: the maintainer reports that the final project builds and all checks pass. The latest driver snapshot available for this documentation predates some reported fixes. This document describes the agreed corrected design; it is not an independent audit of an unavailable final revision. See `docs/code-audit.md` for the source-backed checks and historical discrepancies.

## Shared definitions

Kernel and userspace include one checked-in `uapi/edu_uapi.h`. Do not independently recreate command definitions in production clients. The following are the agreed definitions used in the labs; compare them with the final committed header before release.

```c
#define EDU_IOCTL_MAGIC 'E'

struct edu_factorial_req {
    __u32 input;
    __u32 result;
    __u32 reserved[6];
};

struct edu_dma_selftest_req {
    __u32 length;
    __u32 reserved[3];
};

#define EDU_IOCTL_FACTORIAL     _IOWR(EDU_IOCTL_MAGIC, 0x1, struct edu_factorial_req)

#define EDU_IOCTL_DMA_SELFTEST     _IOW(EDU_IOCTL_MAGIC, 0x2, struct edu_dma_selftest_req)

#define EDU_IOCTL_DMA_STREAM_SELFTEST     _IOW(EDU_IOCTL_MAGIC, 0x3, struct edu_dma_selftest_req)
```

The factorial layout is 32 bytes; the DMA request layout is 16 bytes. Reserved fields must be zero. No userspace pointer, CPU physical address, or raw DMA address is exposed inside these requests.

## Commands

| Command | Input | Success |
|---|---|---|
| Factorial | Integer 0..12 | Returns result in the request |
| Coherent DMA self-test | Length 1..4096 | Driver verifies round-trip payload and coherent RX tail |
| Streaming DMA self-test | Length 1..4096 | Driver verifies transferred payload after unmap |

The DMA commands do not copy a result structure back. An ioctl return of zero indicates self-test success.

## Errors

Kernel callbacks return negative errno values. The userspace ioctl wrapper returns -1 and sets errno.

| Error | Meaning |
|---|---|
| ENOTTY | Command not recognized, including command-encoding mismatch |
| EFAULT | User-memory copy failed |
| ERANGE | Factorial input exceeds the supported range |
| EINVAL | Invalid DMA length or nonzero reserved fields |
| ENODEV | Device disconnected at an applicable admission/completion check |
| ETIMEDOUT | Expected completion was not received within its wait interval |
| EIO | Engine fault, invalid completion state, or data mismatch |
| ENOMEM | Allocation/mapping failure, or a documented injected failure |

An interruptible mutex acquisition can also be interrupted; do not promise that this table enumerates every syscall outcome.

An injected EIO is not necessarily a hardware fault. The stream-failure hooks deliberately return errors at selected cleanup boundaries and need not set the hardware fault latch.

## Stale file semantics

A supported, valid operation after completed unbind returns ENODEV. The fd is not forcibly closed, and final release remains safe. Error precedence for malformed commands is separate: the reviewed ioctl checks magic before disconnect, and complete command matching occurs later.

## Timeout semantics

A DMA timeout latches the shared DMA fault state. Further coherent or streaming DMA requests on that binding return EIO without starting a new transfer. Factorial has a separate fault flag, unless an unsupported device event deliberately faults both paths.

Streaming timeout is not a total execution deadline: the driver must drain hardware before unmap/free. A successful hardware completion before disconnect may permit software-only completion afterward.

## Compatibility and diagnosing ENOTTY

Full ioctl values encode type, number, direction, and structure size. Matching only command number 0x3 is insufficient. A pointer type accidentally used in _IOW changes the encoded size.

Use fixed-width fields and keep layouts identical across supported userspace architectures. `compat_ptr_ioctl` is appropriate for the compatible pointer-based request layout, but actual 32-bit runtime support must not be claimed without testing it.

To diagnose ENOTTY, compare the client's sent command with the kernel's received value and compiled expected constant. Then verify the shared header, rebuild both sides, and reload changed module code. The exact cause of the project's final reported ENOTTY incident was not supplied; do not attribute it to a specific stale binary without evidence.
