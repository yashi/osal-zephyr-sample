# Countsem stage harness

This stage calls the Zephyr counting-semaphore provider directly with explicit
OSAL table-slot tokens. It checks initial values, independent slots,
nonblocking and finite timed waits, the `INT32_MAX` cap and Give overflow,
rejection of values above the cap, and quiescent delete/recreate. It is provider
coverage and does not exercise public object allocation, ID management, or lifecycle admission.

Build and run it for each development target from the workspace root. Use
separate pristine build directories and select the development OSAL checkout:

```sh
west build -p always -d ai-context/tasks/2026-09-07-countsem/builds/countsem/native_sim \
  -b native_sim osal-zephyr-sample/apps/countsem -- \
  -DEXTRA_ZEPHYR_MODULES=$PWD/osal-zephyr
python3 osal-zephyr-sample/scripts/run.py --app countsem \
  --log ai-context/tasks/2026-09-07-countsem/builds/countsem/native_sim/run.log -- \
  ai-context/tasks/2026-09-07-countsem/builds/countsem/native_sim/zephyr/zephyr.exe
```

Repeat the same build and runner commands with `qemu_cortex_m3` and
`nucleo_h755zi_q/stm32h755xx/m7`. For QEMU, pass `--qemu` and run
`west build -d <build-directory> -t run` after `--`. For the Nucleo, flash the
image and validate a fresh serial capture with `--log-file`.

The complete stage emits 38 checks. Public lifecycle coverage lives in
`../../tests/zephyr-countsem/`. All provider operations require thread context.
