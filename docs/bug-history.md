# Debugging and Fix History

Each case follows: symptom -> testable hypothesis -> observation -> cause -> fix -> regression test.

Evidence labels matter: a source-review finding is not an observed crash; a user-reported fix is not an independently reproduced run. The maintainer reports the final checks pass. Specific historical observations are retained below without inventing logs.

## 1. Stale file description could outlive freed device state

### Symptom

A lifecycle review exposed a potential use-after-free when an ioctl used file->private_data after remove had freed edu_dev. This was a static finding, not a documented captured crash.

### Testable hypothesis

An open file description outlives the PCI binding, but the driver does not retain its private object.

### Observation

The old fd can enter the ioctl after misc deregistration; even attempting to lock a mutex inside freed edu_dev is unsafe.

### Cause

Hardware lifetime and software-object lifetime were treated as identical.

### Fix

Allocate shared state with kzalloc, maintain a binding kref and one reference per successful open file description, use per-open edu_file, reject hardware access after disconnect, and return references in release. Keep the final callback software-only.

### Regression test

Open multiple files and a dup, unbind while they remain open, require ENODEV from supported stale ioctls, and verify final object release only after the last file description is released.

## 2. Missing file release leaked references

### Symptom

Review found successful opens incrementing kref with no file_operations.release callback.

### Testable hypothesis

Each close leaves a per-open allocation and a device reference behind.

### Observation

The open path allocated edu_file and took kref; no reverse operation existed in the reviewed file.

### Cause

The ownership pair was implemented only on acquisition.

### Fix

Register edu_release; free the per-open context and put exactly its device reference. Do not access hardware in release.

### Regression test

Repeated open/close followed by unbind must release the base object; multiple opens and dup must neither leak nor release it early.

## 3. Post-publication probe failure freed externally owned state

### Symptom

Static review identified direct kfree after an injected failure following misc_register.

### Testable hypothesis

Userspace can acquire a file reference before probe returns success.

### Observation

Publication precedes the step-7 injection, so an open can race with failed-probe unwind.

### Cause

The unwind assumed no external owners existed until probe completed.

### Fix

Use the same fence, deregistration, transaction drain, hardware cleanup, and base-reference put protocol after publication as on normal removal.

### Regression test

Open during the publication window, trigger step 7, then issue a supported ioctl and require ENODEV; close the fd safely and verify final release.

## 4. Disconnect could not wake a waiter holding the same mutex

### Symptom

Review found remove trying to acquire the mutex held across wait_for_completion_timeout before it could publish disconnect and call complete_all.

### Testable hypothesis

Cancellation is ordered after the wait it is meant to cancel.

### Observation

An active ioctl holds the old lock while waiting, so remove cannot reach its wakeup until the operation finishes or times out.

### Cause

One lock combined whole-transaction serialization with short state transitions.

### Fix

Use op_lock for transactions and state_lock for the fence. Publish disconnect and wake under the short state protocol, then wait for op_lock without holding state_lock.

### Regression test

Use the active-operation hold, unbind, require prompt software wakeup and ENODEV, and keep the fd open afterward.

## 5. Operation errors were returned as success

### Symptom

Review found edu_run_factorial_locked computing negative errors but returning zero at its common exit.

### Testable hypothesis

The error is lost between the helper and ioctl.

### Observation

Timeout, fault, and disconnect branches assigned ret, while the final statement was return 0.

### Cause

The result variable was not propagated.

### Fix

Return ret from the helper and preserve it through ioctl without copying a result on failure.

### Regression test

Require ETIMEDOUT followed by EIO on a fresh injected-timeout binding and ENODEV for active disconnect; a normal factorial success alone is insufficient.

## 6. MSI setup succeeded but completion timed out

### Symptom

Logs showed successful MSI setup and a ready miscdevice, but factorial ioctl returned ETIMEDOUT with completion suppression disabled.

### Testable hypothesis

The device is configured for MSI but lacks permission for the device-initiated write that delivers it.

### Observation

The maintainer explicitly reported that adding pci_set_master resolved the failure.

### Cause

The transition from pin-based INTx to message delivery omitted enabling bus mastering.

### Fix

Enable bus mastering during probe before using MSI/DMA and clear it during orderly teardown. Continue device-level ACK in either interrupt mode.

### Regression test

Check MSI Enable and BusMaster while bound, verify factorial results and IRQ counts, then separately verify INTx fallback.

## 7. Coherent allocation ownership was overwritten

### Symptom

Source review found an inline coherent allocation in probe followed by another allocation through edu_dma_alloc.

### Testable hypothesis

The second allocation replaces the only stored CPU pointer and DMA handle of the first.

### Observation

Both allocation sites wrote the same fields; cleanup had only one allocation to free. A failed second allocation could also overwrite the first pointer with NULL.

### Cause

A helper-based setup was integrated without deleting the older inline setup.

### Fix

Use exactly one coherent allocation path and one matching free, retaining the original device, size, CPU address, and DMA address.

### Regression test

Instrument acquisition/free counts for success and step-10 failure; require one matching pair. Subsequent probe success alone does not prove no leak.

## 8. DMA timeout injection was controlled by the factorial path

### Symptom

Source review found test_force_dma_timeout copied into a device field only while factorial ran, then reset before releasing op_lock.

### Testable hypothesis

DMA IRQs inspect a suppression value that normal DMA operations never set.

### Observation

The serialized factorial path resets the flag before any DMA path can acquire op_lock.

### Cause

Fault-injection state was assigned in the wrong operation implementation.

### Fix

Read the read-only DMA suppression parameter in the DMA IRQ branch, or capture it in DMA start/finish only. Always acknowledge the hardware cause.

### Regression test

On separate fresh bindings, require ETIMEDOUT then EIO for both timeout dma and timeout stream; ensure factorial is not accidentally tested instead.

## 9. The streaming test dispatched a factorial ioctl

### Symptom

The test printed scenario=timeout operation=stream, followed by op=factorial input=256 and ERANGE instead of the expected DMA errors.

### Testable hypothesis

The parser knows stream, but run_ioctl falls through to factorial.

### Observation

The captured output explicitly identified factorial input 256; the driver rejects factorial inputs above 12.

### Cause

Dispatch recognized only TEST_DMA, treating TEST_STREAM as the factorial alternative.

### Fix

Handle TEST_DMA and TEST_STREAM in the DMA branch, select the matching command, and reject unknown enum values explicitly. Rebuild the binary actually executed.

### Regression test

Require stream labels and the stream command in smoke/timeout/stale/active tests; timeout must yield ETIMEDOUT then EIO rather than ERANGE.

## 10. Lockdep assertion disagreed with the helper contract

### Symptom

Review found MMIO helpers asserting op_lock despite probe invoking them with state_lock only. No captured lockdep warning was supplied.

### Testable hypothesis

The diagnostic assertion names the transaction lock instead of the short MMIO/state lock.

### Observation

The helper comment required state_lock, and probe held that spinlock at the call sites.

### Cause

The assertion was copied from transaction-level helpers.

### Fix

Assert state_lock in short shared MMIO helpers; retain op_lock assertions in whole-transaction and drain helpers.

### Regression test

Run probe and operations with lockdep enabled and inspect warnings; verify call-site contracts statically as well.

## 11. ENOTTY from the streaming CLI: diagnostic record

### Symptom

edu_ctl dma-stream 256 reported Inappropriate ioctl for device.

### Testable hypothesis

Client dispatch, ioctl encoding, UAPI headers, or loaded module code may not agree.

### Observation

The reviewed driver supports a stream case but returns ENOTTY for an unrecognized magic/full command. The exact mismatching values were not provided.

### Cause

The specific root cause was not established in the supplied conversation. Do not label this incident as a proven stale-binary or macro-size bug.

### Fix

The diagnostic procedure was to compare sent/received/expected command values, check type/number/size/direction, align the shared header and dispatch, rebuild, and reload changed module code. The maintainer later reported all checks passing, without identifying which discrepancy was fixed.

### Regression test

Run both edu_ctl dma-stream and edu_lifetime smoke stream on the same binding; require actual stream dispatch, successful payload verification, and matching ABI constants.

## Additional reviewed corrections

- Reject BAR lengths that do not cover the highest register access, rather than checking only for zero length.
- Use an ordered else-if admission chain so disconnect is not overwritten by a fault error.
- Validate new test parameters and reject unknown injection steps.
- Return only valid irqreturn_t values from an IRQ handler, never -ENODEV.
- Keep final object diagnostics independent of an unretained pdev pointer.
- Remove duplicate definitions and update drain comments to describe both operation cleanup and removal.

These changes address source-level findings. Final-revision confirmation is a separate step from this historical documentation.

## Incidents without an established root cause

An earlier IRQ vector allocation failed with EINVAL. Correct argument ranges, allocation flags, capability state, and the guest implementation were proposed as checks, but no specific cause was reported. It is not presented as a solved root-cause case.
