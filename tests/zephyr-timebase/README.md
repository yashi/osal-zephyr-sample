# Public Zephyr timebase integration tests

This Zephyr-only suite exercises the shared public timebase and timer APIs over
the native Zephyr provider.  It configures two timebase slots, three timer
slots, and two unused ordinary task slots, then calls `OS_API_Init()` once.
Every case uses bounded waits and cleans up the public objects it creates.

The native cases cover the distinct first and periodic delays, tick-ceiling
behavior for a one-microsecond request, stop and repeated one-shot operation,
no callback before source configuration, and accumulated freerun time while a
callback delays the helper across several native expiries.  A separate worker
calls stop while the callback deliberately holds the helper handoff, then must
complete only after the callback is released.  The lifecycle cases
cover table exhaustion, stale IDs, repeated helper reclamation and slot reuse,
and failed timebase deletion with `OS_ERR_OBJECT_IN_USE` while an attached
timer holds a reference.  That failed delete must leave the helper live and
delivering callbacks.  Repeated armed dedicated `OS_TimerCreate()` deletion
checks that its live hidden timebase is also reclaimed.  A fault-injected helper
registration failure must unwind creation and permit immediate recreation.

The callback-context case requires `OS_TaskGetId()` to identify the helper as
the owning timebase.  It also checks that application custom data survives and
that timebase/timer configuration APIs return
`OS_ERR_INCORRECT_OBJ_STATE`.  The external-sync case blocks on static Zephyr
semaphores, verifies that `OS_TimeBaseSet()` has no source effect, and requires
delete to return `OS_ERROR` while the external function is active.  Teardown is
cooperative: the test releases the external function and then retries deletion
at a safe helper boundary.  A separate case returns `UINT32_MAX` elapsed units
and requires exact freerun accounting after bounded native delivery chunks.

Linker wrappers count calls to `k_malloc()` and `k_free()` without changing
their results.  Snapshots around public creation, deletion, and repeated reuse
require the production path to make no heap calls.  The fixture itself uses
only static objects.

Timing checks use short real delays with broad scheduling bounds.  They verify
ordering and elapsed accounting rather than exact wall-clock callback times.
The suite is restricted to the configured uniprocessor targets; it does not
claim SMP coverage or a formal hard real-time bound.

Run the host targets with the development module selected exactly once:

```sh
west twister -T osal-zephyr-sample/tests/zephyr-timebase \
  -p native_sim -p qemu_cortex_m3 -O "$task_dir/builds/timebase-tests" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

For the Nucleo H755 M7, use device testing and preserve the Twister result,
handler log, flash result, and fresh serial capture.  Verify `OSAL_SOURCE_DIR`
selects `osal-zephyr` and `zephyr_modules.txt` contains one `osal` entry.
