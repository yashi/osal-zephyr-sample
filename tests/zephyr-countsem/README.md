# Public counting-semaphore lifecycle tests

This Zephyr-only fixture tests the public OSAL counting-semaphore API using
three provider slots.  It requires the guarded provider design: every slot has
a permanent mutex, native `k_sem`, full active object ID, and atomic admitted
user count.  The fixture calls `OS_API_Init()` once and cleans every public
object in its own case.

The test wraps only four production boundaries: successful shared `NONE`
lookup, `z_impl_k_mutex_lock`, `z_impl_k_sem_take`, and
`z_impl_k_sem_give`.  A wrapper can gate a named worker after the real shared
lookup, before its real state-mutex acquisition, before native take, or after
the native take has returned but before the provider can release its atomic
reservation.  It never changes a lock mode, return code, provider field, or
kernel wait queue.  All ordering uses semaphore gates and bounded five-second
joins. Three independent worker contexts have separate reached, resume,
native-entry, and completion gates. The controller priority is 2 and worker
priority is 1 on UP targets. The higher-priority worker enters its real
native wait before the controller resumes. The controller owns no provider
mutex at these boundaries, so it cannot inherit a competing priority. The test never examines a private kernel wait queue.

The cases cover public basic operations and argument errors; `INT32_MAX`
capacity and overflow; all-three-slot exhaustion; stale Give/Take/TimedWait
after shared validation and before state admission; admitted wait/delete
quiescence; real token wakeup; timeout accounting across state admission;
three native waiters; concurrent capacity races; and object-core preservation
through repeated quiescent reuse.  The object-core case holds two distinct
live neighbour semaphores while reusing the third and uses a bounded locked
type walk to require each live provider semaphore exactly once.

It establishes behavior only for normally completing threads and distinct
full IDs on the configured UP targets.  It does not claim forced task
retirement, full ID generation wrap, or SMP correctness.

Run with a development module selected exactly once:

```sh
west twister -T osal-zephyr-sample/tests/zephyr-countsem \
  -p native_sim -p qemu_cortex_m3 -O "$task_dir/builds/hosts" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

For the Nucleo H755 M7, use device testing and preserve the Twister result,
handler log, flash result, and fresh serial capture.  Verify `OSAL_SOURCE_DIR`
selects `osal-zephyr` and `zephyr_modules.txt` contains one `osal` entry.

The suite contains nine grouped cases. The partial-budget case requests 90 ms,
spends 30 ms in state admission, derives a native lower bound from observed
elapsed time with a three-tick margin, and requires total elapsed time in
70..249 ms. The native upper bound rejects a restarted 90 ms interval; the
lower bound rejects premature expiry. These are scheduling/rounding tolerances,
not a hard real-time guarantee. A separate UINT32_MAX request waits on an empty
semaphore long enough to inspect its actual finite 64-bit tick argument, then
completes via Give without waiting for that duration.

Gate semaphores are initialized once and reset between uses. Object-core
registration stays enabled for semaphores and mutexes; thread registration is
disabled because test thread storage is reused. Eight same-priority multiwaiter
rounds provide deterministic accounting coverage, not randomized stress under
timer load or varying priorities.
