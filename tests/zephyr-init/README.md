# Zephyr initialization and console synchronization test

This focused native Zephyr ztest application exercises the first usable OSAL
initialization slice. It calls the real shared `OS_API_Init()`, uses the real
Zephyr implementation and generic BSP, and accesses private shared interfaces
only where the public API cannot isolate an implementation contract.

The coverage includes tick setup, repeated initialization, external-thread
identity, exact-length raw BSP output, synchronous `OS_printf()` ring-buffer
draining and controls, concurrent complete-record output, explicit
asynchronous-console rejection, recursive BSP and per-object ID locks,
condition-variable state-change waiting, repeated comparisons with the actual
retry attempts 1 through 4 and their quadratic bounded backoffs, and
idle/shutdown notification in both arrival orders. It does not claim public
task or object lifecycle support.

This is a Zephyr-only test application (ztest, Twister metadata, a
board-qualified build): it lives in this harness repository, not in the
`osal-zephyr` checkout, because `osal-zephyr`'s `src/tests/` and other
upstream-shared test trees must stay portable across every BSP and are not a
place for Zephyr-specific build infrastructure. See `apps/init` for the
lightweight, portable-style stage check of initialization; this suite
supplements it with the concurrency and synchronization contracts that a
single-threaded stage application cannot exercise.

## Build and run

Run commands from the west workspace root with the west environment active.
For a local development checkout, pass its absolute path as an extra module;
the checkout declares the stable module name `osal`, so Zephyr uses it in
preference to the manifest's west-managed `osal` checkout.

```sh
west build -p always -d build/zephyr-init-native -b native_sim \
  osal-zephyr-sample/tests/zephyr-init -- \
  -DEXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
west build -d build/zephyr-init-native -t run
```

Use separate pristine build directories for `qemu_cortex_m3` and
`nucleo_h755zi_q/stm32h755xx/m7`. Verify `OSAL_SOURCE_DIR` in each
`CMakeCache.txt` and confirm that `zephyr_modules.txt` contains exactly one
`osal` module before accepting results.

ztest reports its own pass/fail status and process exit code; this
application does not use `scripts/run.py`. Twister can also discover it via
`tests.yaml`, which allows the three development targets.
