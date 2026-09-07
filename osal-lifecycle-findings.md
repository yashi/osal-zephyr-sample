# OSAL lifecycle findings for the Zephyr port

Engineering notes for Open Source Summit Japan 2026, recorded on 2026-09-07.
This document distinguishes native provider protection from remaining shared
OSAL defects. The immediate plan is to merge the Zephyr port, then propose
shared-layer fixes and portable tests through the upstream process.

## The admission gap

Shared OSAL validates binary-semaphore Give/Take/Flush/TimedWait, mutex
Give/Take, and counting-semaphore Give/Take/TimedWait using
`OS_LOCK_MODE_NONE`. The resulting token contains a full
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
| Condition-variable lifecycle suite | 14 | Passed |
| Existing condition-variable focused suite | 2 | Passed |
| Existing condition-variable stage | 29 | Passed |
| Counting-semaphore public suite | 9 grouped cases | Passed |
| Counting-semaphore stage | 38 | Passed for initial and guarded providers |
| Reset-only counting-semaphore control | 1 | Rejected the unsafe candidate |

All builds selected the development OSAL module exactly once. The lifecycle
fixtures use real public OSAL APIs and native kernel operations, with wrappers
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
- Lifecycle fixtures use one slot and one worker at a time, with SMP, shell
  and networking disabled. The existing condition-variable suites separately
  exercise three waiters. These do not establish multiwaiter/deleter stress
  coverage or priority-inversion performance.
- The full `OS_API_Teardown()` probe cannot yet link because unrelated
  providers remain unimplemented. Manual cleanup and quiescent re-init do
  not establish active-user recovery or full teardown correctness.
- The untimed condition-variable follow-up below retains ownership through
  Wait; timed waits and forced cancellation remain separate work.

After the Zephyr port merges, agree on the shared deletion/cancellation
contract, implement admission and lifetime protection before shared state
access, and add portable tests for all supported ports. Until then, native
provider protection must not be presented as complete shared-object safety.


## Condition-variable follow-up

The untimed condition-variable guard preserves the native application mutex,
condition variable and a separate internal state mutex across logical reuse.
The extra state mutex lets Signal/Broadcast validate the full ID through native
notification without waiting for another thread's application ownership.

Delete uses nonblocking probes for both mutexes, then rejects any outstanding
application depth or wait reservation with `OS_ERROR`. This preserves the
original object on deletion failure. A successful Lock retains native ownership;
Wait converts its single application acquisition into a reservation before
atomically releasing the native mutex. The reservation remains until native
reacquisition finishes and application depth is restored. Deletion is therefore
rejected even when all waiters are asleep and the application mutex is free.

Wait without ownership or with recursive depth other than one is rejected
without losing existing acquisitions. Zephyr's native Wait releases one level
only; accepting a recursively held mutex would leave the outer level held and
prevent ordinary predicate handoff. TimedWait continues to return
`OS_ERR_NOT_IMPLEMENTED` without changing application ownership.

Unlike the mutex Give path, shared condvar operations do not mutate per-slot
owner metadata before provider entry. The Zephyr guard can therefore cover
these untimed operations for distinct full IDs and normally completing callers
without a shared-source change. Native thread termination during a reserved
wait would strand that reservation; task retirement must release it as part
of the deferred E5 design. This guard does not claim cancellation safety.

The [condition-variable lifecycle suite](tests/zephyr-condvar-lifecycle/README.md)
passed fourteen cases on native_sim, QEMU Cortex-M3 and a freshly flashed
Nucleo H755 M7. Its native-notification counters confirm that stale calls do
not reach Signal/Broadcast after reuse. It isolates both waiter reservation
and state-lock protection, including the interval before native reacquisition
with no application owner. The existing two focused cases and twenty-nine
stage checks also passed on each target, including three-worker signal and
broadcast behavior. The old focused fixture no longer calls Create as recovery
after each test, which could reset storage despite remaining ownership.

This increment received independent source and evidence review. It remains
limited to distinct full IDs and normally completing untimed operations.


## Counting-semaphore follow-up

Counting semaphores use a native Zephyr `k_sem` and a permanent per-slot
mutex. The shared Give/Take/TimedWait wrappers supply an unpinned token but
do not access mutable count metadata before provider entry. Validate the
full ID under the permanent mutex before touching the native semaphore.

A Take reserves an atomic admitted-user count before releasing that mutex
and entering its native wait. Its final object access releases the reservation
after the native operation returns. Delete tries the mutex without waiting
and returns `OS_SEM_FAILURE` if the mutex is busy or admitted users remain;
the shared finalizer restores the original public ID. No reset is used to
cancel waiters. Once every admitted operation finishes, deletion and reuse
can proceed. This differs from the binary-semaphore guard, which can invalidate
sleeping waiters and wake them to reject their old ID.

Give holds state through the full-ID check, overflow check, and native Give.
The maximum is `INT32_MAX`, keeping GetInfo's signed count representable.
Larger initial values return `OS_INVALID_SEM_VALUE`; Give at the maximum
returns `OS_SEM_FAILURE` instead of silently saturating. With givers serialized,
concurrent takers only decrease the count. The reviewed evidence is for UP
targets and does not establish SMP acceptance or ISR-safe public operations.

TimedWait uses one deadline for state admission and native waiting. The final
atomic release requires no cleanup mutex that could extend a timeout. Normal
scheduling and tick rounding still apply. Positive finite waits without a
system clock are unsupported; with 32-bit native timeouts, requests above a
conservative `INT32_MAX` tick conversion are unsupported. Both cases return
`OS_ERR_NOT_IMPLEMENTED`. The required targets use clocks and 64-bit timeouts.

The permanent mutex is initialized only once. Native semaphore initialization
on reuse occurs only after quiescence. When Zephyr object cores are enabled,
Delete uses the public unlink API before another Create registers the same
storage. Reinitializing a still-registered native object would damage the
registry even without active waiters; this is a separate native lifecycle
obligation discovered during implementation review.

A local reset-only negative control reproduced the admission gap on all three
targets: an old call passed its deleted-state check, paused before native Take,
then consumed a replacement token supplied after reset. The experiment passes
by detecting the rejected candidate's failure, not by establishing provider
safety. Initial and guarded provider stage snapshots separately passed 38
checks per target. The [public counting-semaphore suite](tests/zephyr-countsem/README.md)
then passed nine grouped cases per target on native_sim, QEMU Cortex-M3 and
freshly flashed Nucleo H755 M7. It forces stale Give/Take/TimedWait calls at
both admission boundaries; holds reservations before native entry and after
success/timeout; overlaps real givers and native Take/Give; verifies partial
timeout budgets and wide finite conversion; and checks registry membership
with two live neighboring objects through repeated reuse. Eight deterministic
three-waiter rounds verify exact permit accounting. These are controlled
interleavings, not randomized stress with varying priorities or timer load.

Forced native termination while admitted would strand the reservation;
termination while holding state could strand the mutex. Task retirement,
full ID wrap, SMP and nonquiescent teardown remain deferred. The shared-layer
fix stays scheduled after the Zephyr port merges.
