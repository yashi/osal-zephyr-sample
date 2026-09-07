# Condition-variable lifecycle regressions

This Zephyr-only public-API fixture limits condition variables to one physical
slot. It validates the permanent-native-storage contract: the application
mutex, lifecycle state mutex, and condition variable survive logical deletion;
the provider compares the complete object ID while it holds the relevant
native lock; application depth and waiter reservations prevent deletion while
an old call still has a claim on the slot.

The fixture wraps successful shared `NONE` validation, the real Zephyr mutex
entry point, the real condition-variable wait boundary, and native notifier
calls. Wrappers only add worker-specific semaphore gates or call counters.
They never alter a lock mode, return value, task identity, or native
notification result, and never directly write provider/native state. The
stale-Unlock lookup callback deliberately invokes public Delete/Create/Lock
to replace the object before the old operation reaches the provider.

The fourteen cases cover:

- stale Lock calls paused after shared validation and before the application
  mutex, including twelve deterministic same-slot reuse rounds;
- stale Signal and Broadcast calls paused before either provider state lock,
  proving the replacement receives no native notification;
- a same-thread stale Unlock callback that deletes, recreates, and locks the
  replacement before the old Unlock reaches the provider;
- Delete rejection for self, other-thread, and recursive ownership, plus a
  queued native-Lock-to-provider-depth handoff boundary;
- Wait rejection without ownership, from a non-owner, and with recursion;
- a single Wait from shared validation through real atomic sleep, native wake
  reacquisition, provider bookkeeping, public return, and final Unlock;
- Signal and Broadcast from a worker while the controller owns the application
  mutex. Each pauses inside its real native notification while `state_lock` is
  held; prompt failed Delete and unchanged-ID checks demonstrate that
  notification depends only on state synchronization. A separate Signal case
  leaves the application mutex free to isolate the state-lock rejection; and
- worker termination after shared validation but before provider admission.

All scheduling assertions use semaphores and five-second bounds. The fixture
requires UP scheduling: controller priority 2 and worker priority 1 ensure a
released worker enters its native wait before the controller continues. It does
not abort a thread while it owns an application mutex or has a waiter
reservation; that retirement contract remains intentionally unsupported.

Timed wait remains `OS_ERR_NOT_IMPLEMENTED` and is covered by the existing
focused condition-variable suite, which also checks that it preserves the
caller's application ownership.

## Running

```sh
west twister -T osal-zephyr-sample/tests/zephyr-condvar-lifecycle \
  -p native_sim -p qemu_cortex_m3 \
  -O "$task_dir/builds/hosts" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"

west twister -T osal-zephyr-sample/tests/zephyr-condvar-lifecycle \
  -p nucleo_h755zi_q/stm32h755xx/m7 \
  --device-testing --hardware-map hwmap.yaml \
  -O "$task_dir/builds/nucleo" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

Verify that `OSAL_SOURCE_DIR` selects `osal-zephyr` and that
`zephyr_modules.txt` has one `osal` entry. Preserve Twister JSON, handler
logs, and fresh serial output with the task evidence.
