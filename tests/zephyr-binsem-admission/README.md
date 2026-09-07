# Binary-semaphore lifecycle regressions

This Zephyr-only fixture exercises the public binary-semaphore API against a
single physical provider slot. It validates the permanent-native-storage
contract: native mutex and condition-variable objects survive slot reuse, each
operation checks the full object identity while holding the provider mutex,
and a waiter checks it again after every wake.

The test application wraps only shared-ID validation and the two relevant real
Zephyr kernel implementation entry points. The shared-ID wrapper pauses a worker only
after a successful validation; it does not modify its lock mode. The mutex
and condition-variable wrappers provide barrier-backed scheduling points,
including the mutex reacquisition in `condvar.c` after a wake. No provider
return values, shared references, or waiter counters are emulated here.

## Cases

The suite has fourteen ztest cases:

- An old `Give` paused after validation and before the provider mutex is
  deleted and replaced. It returns `OS_ERR_INVALID_ID`; the new semaphore
  remains empty in both interleavings.
- Stopping a native worker after validation leaves no admission state behind;
  deletion and same-slot recreation then succeed.
- A waiter paused while it owns the provider mutex makes public `Delete`
  return `OS_SEM_FAILURE`, and the original ID remains active.
- Once a waiter has entered the condition variable's atomic unlock-and-wait,
  the controller holds the persistent mutex while public `Delete` and `Create`
  recursively take it. The old waiter is paused at condvar's post-wake mutex
  acquisition, then returns `OS_ERR_INVALID_ID` without consuming the new
  semaphore's token.
- A native worker stopped while it is in the real condition-variable wait owns
  no provider mutex; deletion and reuse succeed.
- Normal timeout and a timeout that completes before its mutex reacquisition
  preserve binary-semaphore semantics. In the latter case, deletion and reuse
  make the old operation return `OS_ERR_INVALID_ID` rather than its stale
  timeout result, and the replacement's token remains available.
- Flush of a current waiter, flush of an available semaphore, and give of a
  current waiter preserve binary-semaphore semantics.
- A zero-duration timed wait contending for the provider mutex returns
  `OS_SEM_TIMEOUT` before the controller releases that mutex.
- Manual deletion of the only binary semaphore followed by quiescent
  `OS_ApplicationShutdown(true)` and `OS_API_Init()` leaves permanent provider
  storage usable: a new full semaphore is created, consumed, and deleted.
  This is not `OS_API_Teardown()` or full-cleanup API coverage, does not cover
  shutdown with active users, and does not require object IDs to differ across
  an API reset.
- The post-wake delete/recreate interleaving runs for twelve deterministic
  rounds. Barriers and bounded joins establish every ordering; sleeps are not
  used as ordering evidence.

The fixture checks the changed full object ID and identical physical index on
each reuse. It assumes `OS_MAX_BIN_SEMAPHORES == 1`, uses a 5-second barrier
and join watchdog, and gives workers 2048 bytes of stack. It targets UP
platforms in this workspace; the controller's persistent mutex ownership is
used to serialize the atomic-wait handoff.

## Running

From the workspace root, with a new absolute output directory:

```sh
west twister -T osal-zephyr-sample/tests/zephyr-binsem-admission \
  -p native_sim -p qemu_cortex_m3 \
  -O "$task_dir/builds/hosts" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"

west twister -T osal-zephyr-sample/tests/zephyr-binsem-admission \
  -p nucleo_h755zi_q/stm32h755xx/m7 \
  --device-testing --hardware-map hwmap.yaml \
  -O "$task_dir/builds/nucleo" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

Expect fourteen cases per target, no skips, a complete run ID, and successful
process or flash status. Preserve Twister JSON, handler logs, and device logs.
Verify `OSAL_SOURCE_DIR` points at `osal-zephyr` and `zephyr_modules.txt`
contains exactly one `osal` entry.
