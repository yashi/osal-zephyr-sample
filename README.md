# OSAL Zephyr development harness

This west manifest repository holds small applications used while developing
`osal-zephyr`. Add or change applications as the port gains capabilities.
Each application checks a specific stage; a passing result applies only to
that application's stated scope.

## Applications

| Path | Scope |
|---|---|
| `apps/basic` | Link the real OSAL and BSP libraries, check the version accessors against their headers, and check the BSP's initial argument state |
| `apps/binsem` | Call the Zephyr binary semaphore creation provider and inspect the created native semaphore objects |
| `apps/init` | Call real OSAL initialization, check tick values and console identity/draining, repeat initialization, and request shutdown before entering idle |

`basic` does not call `OS_API_Init()` or `OS_Application_Startup()`. It makes
initial build integration independently checkable without depending on
the later initialization or object providers.

`binsem` uses OSAL private headers and calls `OS_BinSemCreate_Impl()` with
explicit table-slot tokens. It checks creation using Zephyr's native
semaphore accessors. It does not exercise OSAL public semaphore allocation,
ID management, give/take, timeout, flush, or deletion. Keep this scope clear
when extending it to later stages. The production provider is linked from
the selected OSAL module; its source is not copied into the harness.

`init` requires the initialization and synchronous console providers. Its
ten checks cover initialization and shutdown signaling, not public task or
semaphore lifecycle, clocks, timers, or object teardown. Focused provider and
concurrency tests live in the OSAL checkout at `src/tests/zephyr-init`.

## Build a stage

Run commands from the west workspace root with the west environment active.
Choose an application directory explicitly; the repository root is no longer
a Zephyr application.

For a local development checkout, pass its absolute path as an extra module.
The development checkout declares the stable module name `osal`, so Zephyr
uses it in preference to the manifest's west-managed `osal` checkout instead
of loading duplicate OSAL modules.

```sh
west build -p always -d build/basic-native -b native_sim \
  osal-zephyr-sample/apps/basic -- \
  -DEXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

Select `apps/binsem` for creation checks. Use separate pristine build
directories for each application, revision, and target. The three
development targets are:

- `native_sim`
- `qemu_cortex_m3`
- `nucleo_h755zi_q/stm32h755xx/m7`

Check `CMakeCache.txt`: `OSAL_SOURCE_DIR` must resolve to the intended OSAL
checkout. Check `zephyr_modules.txt` for exactly one `osal` entry. The
manifest supplies the normal OSAL checkout when the extra-module override
is omitted; record which source was actually selected.

## Run and inspect results

Each application emits a start record, individual check results, and one
aggregate result. A nonempty set of passing checks and a complete matching
result are required. The runner also rejects inconsistent counts, duplicate
checks, incomplete output, and a nonzero host process status.

Native simulation exits with the application result:

```sh
python3 osal-zephyr-sample/scripts/run.py --app basic \
  --log build/basic-native/run.log -- \
  build/basic-native/zephyr/zephyr.exe
```

For a QEMU build, the runner waits for the application result and then sends
QEMU's quit sequence. Its controlled emulator exit is separate from the
application outcome verified in the output:

```sh
python3 osal-zephyr-sample/scripts/run.py --app basic --qemu \
  --log build/basic-qemu/run.log -- \
  west build -d build/basic-qemu -t run
```

Use the selected application directory name as the `--app` value and use its
matching build directory.
A timeout is a failure; increase `--timeout` explicitly when investigating
slow runs.

For Nucleo, start serial capture before flashing:

```sh
tio /dev/ttyACM0 -L --log-file build/basic-nucleo/run.log
```

In another terminal, run `west flash -d build/basic-nucleo -r openocd`.
After the fresh boot and complete result, exit tio and validate the capture:

```sh
python3 osal-zephyr-sample/scripts/run.py --app basic \
  --log-file build/basic-nucleo/run.log
```

Require a successful flash as well as a passing capture from that run.
A hardware board has no process exit status to substitute for its result.
Use a fresh log for each run; the validator rejects multiple result sets.

## Record the stage inputs

Record the OSAL revision, harness revision and any uncommitted patch,
application path, board, configuration, toolchain, and raw output for each
run. One harness revision can contain applications for several OSAL stages;
select only the applications supported by the OSAL revision under review.

The two repositories do not need matching commit counts. During OSAL
history cleanup, select each candidate through the module override. Update
the manifest pin after the final OSAL revision is settled.
