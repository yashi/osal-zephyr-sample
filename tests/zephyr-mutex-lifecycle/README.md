# Mutex lifecycle regressions

This Zephyr-only fixture limits mutex storage to one physical slot and uses
only public mutex APIs with real kernel scheduling gates. It checks that the
permanent native mutex, full object ID, active flag, and application depth
prevent a stale operation from acquiring or releasing replacement native
application ownership. Full public whole-object safety remains open because
shared-layer state can change before the Zephyr provider is entered.

The fixture wraps `OS_ObjectIdGetById` after successful mutex validation and
the real Zephyr `z_impl_k_mutex_lock` entry point. The wrappers only provide
worker-specific, semaphore-backed barriers; they do not change shared lock
modes, return values, task IDs, or provider behavior.

## Cases

- Stale `Take` after validation and before the native lock, plus stale `Give`,
  are deleted and recreated in the single slot and return `OS_ERR_INVALID_ID`.
- A same-thread stale `Give` callback deletes and recreates the slot, takes the
  replacement, and proves the old Give cannot reduce the replacement depth.
- Recursive ownership prevents public Delete by both owner and another thread
  before either Give, and after the first of two Gives; Delete succeeds only
  after the final Give.
- A queued Take is stopped before native acquisition, after Zephyr's native
  handoff but before provider validation, and after successful public Take.
  Delete fails at each boundary; the final barrier follows successful public
  Take, then the worker gives.
- Wrong-owner Give, ordinary contention, abort after validation with no native
  ownership, and twelve deterministic stale-Take reuse rounds cover ordinary
  provider paths without sleeps as ordering evidence.
- `test_known_shared_last_owner_clear_before_stale_give_reject` is a diagnostic
  for an existing shared-layer gap. It writes a nonzero sentinel into the real
  shared `last_owner` slot, then observes that shared `OS_MutSemGive` clears it
  before the Zephyr provider rejects the stale ID. It is not a safety pass and
  does not fake a task identity.

Workers use 2048-byte stacks, all barriers and joins have a five-second bound,
and the handoff cases require UP scheduling. Controller priority 2 and worker
priority 1 ensure a resumed worker enters the native wait before the controller
can release its acquisition; compile-time checks enforce these assumptions. Native workers are stopped only
after shared validation, before they own a native mutex.

## Running

```sh
west twister -T osal-zephyr-sample/tests/zephyr-mutex-lifecycle \
  -p native_sim -p qemu_cortex_m3 \
  -O "$task_dir/builds/hosts" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"

west twister -T osal-zephyr-sample/tests/zephyr-mutex-lifecycle \
  -p nucleo_h755zi_q/stm32h755xx/m7 \
  --device-testing --hardware-map hwmap.yaml \
  -O "$task_dir/builds/nucleo" \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

Verify the selected module is `osal-zephyr` and `zephyr_modules.txt` has one
`osal` entry. Preserve Twister JSON, handler logs, and device logs.
