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
| `apps/clock` | Exercise native monotonic and settable local clocks, including forward and backward local-time changes |
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
concurrency tests live in this repository at `tests/zephyr-init`: they are a
Zephyr-only ztest application, so they belong in the harness rather than in
`osal-zephyr`'s upstream-shared test trees.

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

## Zephyr-only test suites (`tests/`)

`tests/` holds Zephyr-only ztest/Twister suites, as opposed to the hand-rolled
stage applications above. Twister discovers every suite under the directory
automatically, so adding a new suite (a `tests.yaml`/`testcase.yaml`,
`CMakeLists.txt`, and `prj.conf` alongside its sources, following the
`tests/zephyr-init` layout) needs no changes anywhere else to be picked up.

Each suite's `tests.yaml` lists the three development targets under both
`platform_allow` and `integration_platforms`, so the board list lives in the
test file, not on the command line.

Run every suite on `native_sim` and `qemu_cortex_m3` (both build and execute
their checks through the `ztest` harness) with:

```sh
west twister -T osal-zephyr-sample/tests -G \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

`-G`/`--integration` runs each suite's `integration_platforms` list instead of
Twister's own per-arch default platform, which is otherwise just
`native_sim/native`. This same invocation also builds (but, with no hardware
attached, does not run) the `nucleo_h755zi_q/stm32h755xx/m7` configuration, so
a build regression on that target is still caught.

### Run on Nucleo hardware

Twister can flash and validate the real board itself, replacing the manual
`tio`/`west flash` capture used for the stage applications. First generate a
hardware map once per machine:

```sh
west twister --generate-hardware-map hwmap.yaml
```

Edit the generated entry's `platform:` field from `unknown` to
`nucleo_h755zi_q/stm32h755xx/m7`; `runner: openocd` and the `/dev/ttyACM0`
serial device are detected correctly. Then run:

```sh
west twister -T osal-zephyr-sample/tests -G \
  -x=EXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr" \
  --device-testing --hardware-map hwmap.yaml
```

`--device-testing` restricts the run to platforms present in the hardware
map, so this flashes and validates only `nucleo_h755zi_q/stm32h755xx/m7`
over serial; it does not also run the `native_sim`/`qemu_cortex_m3`
configurations in the same invocation. Use the two commands above together
to cover all three targets. Consult Twister's generated results instead of a
README check total, because the total changes whenever suites or cases are
added.

## Record the stage inputs

Record the OSAL revision, harness revision and any uncommitted patch,
application path, board, configuration, toolchain, and raw output for each
run. One harness revision can contain applications for several OSAL stages;
select only the applications supported by the OSAL revision under review.

The two repositories do not need matching commit counts. During OSAL
history cleanup, select each candidate through the module override. Update
the manifest pin after the final OSAL revision is settled.
