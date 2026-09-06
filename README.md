# OSAL Zephyr development harness

This west manifest repository holds small applications used while developing
`osal-zephyr`. Add or change applications as the port gains capabilities.
Each application checks a specific stage; a passing result applies only to
that application's stated scope.

## Applications

| Path | Scope |
|---|---|
| `apps/basic` | Link the real OSAL and BSP libraries, check the version accessors against their headers, and check the BSP's initial argument state |

`basic` does not call `OS_API_Init()` or `OS_Application_Startup()`. The
current port cannot complete that initialization. This application makes
initial build integration independently checkable without depending on
unfinished task, console, or synchronization providers.

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

Use separate pristine build directories for each application,
revision, and target. The three development targets are:

- `native_sim`
- `qemu_cortex_m3`
- `nucleo_h755zi_q/stm32h755xx/m7`

Check `CMakeCache.txt`: `OSAL_SOURCE_DIR` must resolve to the intended OSAL
checkout. Check `zephyr_modules.txt` for exactly one `osal` entry. The
manifest supplies the normal OSAL checkout when the extra-module override
is omitted; record which source was actually selected.

## Run and inspect results

Each application emits a start record, individual check results, and one
aggregate result. Require a nonempty set of passing checks and a complete
matching result; a boot banner alone is insufficient.

Run a native build directly and require process status zero:

```sh
build/basic-native/zephyr/zephyr.exe
```

Run a QEMU build with `west build -d build/basic-qemu -t run`. After the
complete application result, quit QEMU with Ctrl-A x. That controlled
emulator exit is separate from the application outcome printed in its
result records.

For Nucleo, start `tio /dev/ttyACM0` before flashing with
`west flash -d build/basic-nucleo -r openocd` in another terminal. Require
a successful flash and the complete application result from the fresh boot.

## Record the stage inputs

Record the OSAL revision, harness revision and any uncommitted patch,
application path, board, configuration, toolchain, and raw output for each
run. One harness revision can contain applications for several OSAL stages;
select only the applications supported by the OSAL revision under review.

The two repositories do not need matching commit counts. During OSAL
history cleanup, select each candidate through the module override. Update
the manifest pin after the final OSAL revision is settled.
