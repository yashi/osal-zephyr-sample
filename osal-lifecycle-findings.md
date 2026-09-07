# OSAL lifecycle findings for the Zephyr port

Engineering notes for Open Source Summit Japan 2026, recorded on 2026-09-07.
This document distinguishes native provider protection from remaining shared
OSAL defects. The immediate plan is to merge the Zephyr port, then propose
shared-layer fixes and portable tests through the upstream process.

## The admission gap

Shared OSAL validates binary-semaphore Give/Take/Flush/TimedWait and mutex
Give/Take using `OS_LOCK_MODE_NONE`. The resulting token contains a full
object ID and a physical table index, but does not retain the object's
lifetime. A thread can pause after validation, another thread can delete and
recreate the slot, and the original operation can resume against replacement
storage.

The original Zephyr binary-semaphore experiment paused Give after shared ID
validation, deleted the old semaphore, and created an empty semaphore in the
same slot with a distinct ID. Resuming the old Give made the replacement
available. The experiment also reproduced the failure when pausing before
the provider mutex acquisition. This was a static-slot generation failure,
not evidence of heap memory being freed and accessed afterward.

A test-only shared-reference prototype protected normal completion, but
stopping a native worker after admission left its reference unreleased.
Reference acquisition therefore needs an ownership and cancellation protocol;
a normal-return release alone is insufficient.

These observations concern the tested Zephyr port. They neither establish
that every OSAL port reproduces the defect nor settle the upstream contract
for deleting an object concurrently with its use.

## Binary-semaphore protection

The Zephyr provider now initializes each slot's native mutex and condition
variable once and preserves them across deletion and recreation. Under that
mutex, it compares the operation's full ID with the active ID before reading
or modifying semaphore state. Waiters repeat the comparison after every wake,
before interpreting timeout status or consuming a token.

Delete fails with `OS_SEM_FAILURE` when another thread owns the internal
mutex. Otherwise it marks the object inactive and wakes old waiters. Those
waiters reject a distinct replacement ID without consuming its token. No
manual waiter count or shared reference is retained across a kernel wait.

The [binary-semaphore suite](tests/zephyr-binsem-admission/README.md) contains
14 regressions covering delayed calls, deletion rollback, sleeping waiters,
completed timeouts before mutex reacquisition, ordinary Give/Flush/timeout
semantics, repeated reuse and bounded quiescent shutdown/reinitialization.

## Mutex protection and the remaining shared defect

Mutex ownership spans Take and Give. The provider therefore rejects deletion
while application ownership remains, including recursive ownership by the
thread requesting deletion. It keeps the native mutex permanently initialized
and protects its active ID and application recursion depth with that mutex.
Take validates the ID after native acquisition. Give uses a nonblocking
recursive probe to validate the ID and depth before releasing an acquisition.
Delete uses a nonblocking probe and requires application depth zero.

Zephyr transfers native ownership directly to a queued waiter. Delete also
fails after that handoff but before the returning Take records its application
depth. The implementation retains native recursive locking and priority
inheritance without inspecting private kernel ownership fields.

However, shared `OS_MutSemGive()` in OSAL's
`src/os/shared/src/osapi-mutex.c` clears `last_owner` **before** calling the
provider. A stale Give can still clear the replacement's shared owner record,
even though the provider rejects it. Native checks cannot prevent an earlier
shared write. This remains an unresolved shared-layer defect.

The [mutex suite](tests/zephyr-mutex-lifecycle/README.md) has ten native
protection tests and one explicitly named known-defect diagnostic. Because
the current Zephyr task provider returns an undefined OSAL ID for external
threads, the diagnostic seeds a nonzero owner value in an idle replacement's
shared record to expose the write. It does not fake a task identity. A
separate same-thread case demonstrates that a stale Give cannot release a
replacement mutex that the same thread has since acquired.

## Validation

Every suite below built and ran on `native_sim`, `qemu_cortex_m3`, and a
freshly flashed `nucleo_h755zi_q/stm32h755xx/m7` with serial validation:

| Coverage | Cases or checks per target | Result |
| --- | ---: | --- |
| Binary-semaphore lifecycle suite | 14 | Passed |
| Existing binary-semaphore stage | 31 | Passed |
| Mutex native protection | 10 | Passed |
| Mutex shared-owner diagnostic | 1 | Confirmed the remaining defect |
| Existing mutex stage | 17 | Passed |

All builds selected the development OSAL module exactly once. The fixtures
use real public OSAL APIs and native kernel operations, with linker wrappers
providing deterministic scheduling barriers. Provider return values and
shared admission modes are not mocked. The mutex handoff test requires UP
scheduling and a worker with higher priority than the controller.

These results were independently reviewed. They establish bounded behavior,
not full object-lifecycle acceptance. The original nine-case experiment also
ran on all three targets; its successful negative controls reproduced the
original defects and must not be counted as safety regressions.

## Open findings and upstream follow-up

- Shared `last_owner` mutation remains outside provider protection. Shared
  queue Get/Put also read `max_size` before provider entry and need review.
- Full public IDs have a finite 16-bit serial component. An operation that
  survives an entire allocation cycle can become indistinguishable from a
  replacement. The guards protect reuse with distinct full IDs.
- Stopping a native thread while it owns a mutex can strand ownership.
  Public task retirement and cancellation require an ownership-aware design.
- A stale mutex Take can wait behind a replacement owner before rejecting
  its ID. No bounded completion guarantee is established for that case.
- Tests use one slot and one worker at a time, with SMP, shell and networking
  disabled. They do not establish multiwaiter/deleter stress coverage or
  priority-inversion performance.
- The full `OS_API_Teardown()` probe cannot yet link because unrelated
  providers remain unimplemented. Manual cleanup and quiescent re-init do
  not establish active-user recovery or full teardown correctness.
- Condition variables retain application ownership across Lock/Wait/Unlock
  and require their own lifecycle analysis and tests.

After the Zephyr port merges, agree on the shared deletion/cancellation
contract, implement admission and lifetime protection before shared state
access, and add portable tests for all supported ports. Until then, native
provider protection must not be presented as complete shared-object safety.
