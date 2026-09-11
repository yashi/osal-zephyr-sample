# Timebase stage application

This stage exercises the public OSAL timebase and timer APIs with one native
timebase and one attached timer.  It emits 17 `HARNESS CHECK` records covering
initialization, creation, quiet behavior before Set, periodic callbacks,
timebase helper identity, freerun and information queries, stop, one-shot use,
deletion, and stale-ID rejection.

The application configures two timebase slots, three timer slots, two ordinary
task slots, no heap, and a native system clock.  Build with the development OSAL
module selected explicitly:

```sh
west build -p always -d "$task_dir/builds/timebase/native_sim" \
  -b native_sim osal-zephyr-sample/apps/timebase -- \
  -DEXTRA_ZEPHYR_MODULES="$PWD/osal-zephyr"
```

Run the native stage through the common harness validator:

```sh
.venv/bin/python osal-zephyr-sample/scripts/run.py --app timebase \
  --log "$task_dir/builds/timebase/native_sim/run.log" -- \
  "$task_dir/builds/timebase/native_sim/zephyr/zephyr.exe"
```

QEMU and Nucleo builds use the corresponding target names and distinct build
directories from the workspace instructions.
