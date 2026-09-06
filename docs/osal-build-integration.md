# Design note: integrating OSAL into the Zephyr build

## Purpose

This note explains how OSAL targets are integrated into the Zephyr
build, what each target is responsible for, and how Zephyr compile
settings are propagated into OSAL targets.

It also clarifies the difference between compile-setting propagation
and generated-header ordering.

## TL;DR

`zephyr_interface` is for compile settings.

`zephyr_generated_headers` is for generated-header ordering.

`osal` is the main library target applications should link to.

`osal_zephyr_impl` is the internal OS implementation target.

`osal_bsp` is the BSP library target that compiles the sources listed
in `BSP_SRCLIST`.

If an OSAL target should be compiled with the normal Zephyr compile
environment, it must be linked to `zephyr_interface`.

If an OSAL target includes generated Zephyr headers, and build
ordering is not otherwise guaranteed, it may also need an explicit
dependency on `zephyr_generated_headers`.

## Target roles in OSAL

OSAL defines multiple targets with different roles.

- `osal` is the main app-facing OSAL library
- `osal_public_api` is the interface-properties target carried by `osal`
- `osal_zephyr_impl` is the internal OS implementation target
- `osal_bsp` is the BSP library
- `osal_generic-zephyr_impl` is the internal BSP implementation target

`osal` is the main OSAL library target. It is a `STATIC` library built
from the common OSAL sources plus the object files from
`osal_zephyr_impl`.

`osal_public_api` is an `INTERFACE` target that carries OSAL public
API properties such as include directories and related interface usage
requirements.

`osal_zephyr_impl` is an internal `OBJECT` library that compiles
selected OS implementation sources, including portable providers. This
is generalized as `osal_${OSAL_SYSTEM_OSTYPE}_impl` in OSAL
`CMakeLists.txt`.

`osal_generic-zephyr_impl` is an internal `OBJECT` library that
compiles BSP-specific implementation sources. This is generalized as
`osal_${OSAL_SYSTEM_BSPTYPE}_impl` in OSAL `CMakeLists.txt`.

`osal_bsp` is a `STATIC` library built from BSP shared sources plus
the object files from `osal_${OSAL_SYSTEM_BSPTYPE}_impl`.

## Zephyr-side concepts that matter here

Zephyr separates two concerns that are easy to mix up.

`zephyr_interface` is used for compile and build settings such as
include directories, compile definitions, and compile options.

`zephyr_generated_headers` is used for build ordering. It is the
dependency point for targets that include generated Zephyr headers and
therefore must wait until those headers are ready.

So the rule is:

- `zephyr_interface` is for build settings
- `zephyr_generated_headers` is for generated-header ordering

These are separate roles and should be modeled separately in the
integration.

## How the module-side integration works

The Zephyr module entry point is `zephyr/CMakeLists.txt`, as selected
by `zephyr/module.yml`, in the `osal-zephyr` development checkout.

In that file, OSAL first pulls in its top-level build with:

```cmake
add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR} build)
```

After that call returns, targets defined by the top-level OSAL
`CMakeLists.txt`, such as `osal`, `osal_bsp`,
`osal_zephyr_impl`, and `osal_generic-zephyr_impl`, already exist and
can be further integrated on the Zephyr side.

This is important because Zephyr-specific integration steps, such as
appending a library to the Zephyr library list or linking a target
with `zephyr_interface`, must happen after the relevant target has
been created.

## How Zephyr compile settings are propagated

Registering an OSAL target with Zephyr and giving it the Zephyr build
environment are separate steps.

For example, the following pattern makes `osal_bsp` participate in the
Zephyr library graph and inherit Zephyr compile settings:

```cmake
zephyr_append_cmake_library(osal_bsp)
target_link_libraries(osal_bsp PUBLIC zephyr_interface)
```

These two lines matter for different reasons.

`zephyr_append_cmake_library(osal_bsp)` makes `osal_bsp` part of the
Zephyr-managed library set.

`target_link_libraries(osal_bsp PUBLIC zephyr_interface)` gives
`osal_bsp` the normal Zephyr compile environment, including the
relevant include directories, compile definitions, and compile
options.

This distinction matters in practice because the source files listed
in `BSP_SRCLIST`, such as `src/bsp/shared/src/bsp_default_app_startup.c`,
are compiled as part of osal_bsp. Therefore, if those BSP sources
should receive the usual Zephyr compile settings, `osal_bsp` itself
must be linked with `zephyr_interface`.

## Relationship between `osal_public_api` and implementation targets

`osal_public_api` is an `INTERFACE` target. It carries public API
usage requirements.

In OSAL, some interface properties from `osal_public_api` are copied
into implementation targets such as `osal_${OSAL_SYSTEM_BSPTYPE}_impl`
using expressions such as:

```cmake
target_compile_definitions(osal_${OSAL_SYSTEM_BSPTYPE}_impl PRIVATE
    $<TARGET_PROPERTY:osal_public_api,INTERFACE_COMPILE_DEFINITIONS>
)
target_compile_options(osal_${OSAL_SYSTEM_BSPTYPE}_impl PRIVATE
    $<TARGET_PROPERTY:osal_public_api,INTERFACE_COMPILE_OPTIONS>
)
```

This copies selected interface properties from `osal_public_api` into
the concrete implementation target.

However, that is not the same thing as giving the target the full
Zephyr compile environment. The Zephyr build environment comes from
linking the target with `zephyr_interface`.

## When `zephyr_generated_headers` matters

Giving a target `zephyr_interface` provides the compile environment,
but it does not by itself express every ordering dependency.

If a target includes generated Zephyr headers, and those headers may
not yet exist when compilation begins, the target may need an explicit
dependency on `zephyr_generated_headers`:

```cmake
add_dependencies(<target> zephyr_generated_headers)
```

So the intended interpretation is:

- use `zephyr_interface` to provide compile settings
- use `zephyr_generated_headers` only when explicit generated-header
  ordering is needed

These roles should remain separate.

## Which target should get which integration

`osal` is the main application-facing library target.

`osal_zephyr_impl` is an internal implementation target for selected
OS sources, including the portable BSP console output provider.

`osal_bsp` is the target that compiles BSP shared sources and links in
BSP implementation objects.

The current integration applies `zephyr_interface` to all four concrete
OSAL targets: `osal`, `osal_bsp`, `osal_zephyr_impl`, and
`osal_generic-zephyr_impl`. This gives both shared sources and
implementation objects the same target ABI and compile environment as
the application.

Both implementation targets explicitly depend on
`zephyr_generated_headers`. The module registers the `osal` and
`osal_bsp` static libraries with `zephyr_append_cmake_library`.

## Why internal implementation targets should remain internal

`osal_zephyr_impl` and `osal_generic-zephyr_impl` are `OBJECT`
libraries created only to build implementation objects.

They are not the public API surface, and they are not the targets that
applications should link to directly.

So they should remain internal implementation targets in the design.

The main application-facing target remains `osal`, while
implementation-specific targets receive only the Zephyr integration
they need in order to compile correctly.

## Current port scope

The initial integration builds OSAL and BSP libraries for applications
that use the version accessors and the BSP's initial argument state.
The generic BSP provides recursive locking, raw synchronous console output,
and a fatal shutdown hook. Display mode requests are ignored.

`OS_API_Init()` initializes the shared records, per-type table mutexes and
condition variables, tick globals, and the synchronous `OS_printf` console.
The Zephyr default for `OSAL_CONFIG_CONSOLE_ASYNC` is false; command-line
and application configuration overrides retain their normal precedence.
An asynchronous request fails initialization with `OS_ERR_NOT_IMPLEMENTED`.
Console output uses Zephyr's `k_str_out()` to write the supplied byte count,
including embedded zero bytes, synchronously under BSP locking. It bypasses
logging redirection and honors `CONFIG_PRINTK_SYNC` when enabled. The BSP
lock serializes OSAL/BSP writers; printk synchronization protects individual
output calls, not whole records assembled from multiple calls. Other console
writers can still interleave depending on their output path and configuration.
The selected console driver controls newline translation.

The legacy tick globals use `CONFIG_SYS_CLOCK_TICKS_PER_SEC` and whole
microseconds per tick, rounded down when the rate does not divide one
million. Zero and rates above one million are rejected during initialization.
This does not implement the clock or timer APIs.

Table locks cover every object type, including RW locks and reserved ID
types. The backend mutexes are recursive, but shared token ownership is
single-level. State-change waits release and reacquire that level using a
condition variable, with bounded timeouts for the shared retry protocol.
Initialization must be serialized, and reinitialization requires quiescent
callers and no live objects, as the shared layer resets its tables.

The idle implementation supports one application idle thread. A binary
semaphore retains shutdown notification arriving before that thread waits.
`OS_ApplicationShutdown()` wakes it; this does not implement object deletion
or `OS_API_Teardown()`. BSP fatal shutdown is distinct and does not return.

The binary semaphore provider supplies creation only. It initializes a
native `sys_sem` in an explicitly selected OSAL table slot, normalizes a
nonzero initial value to one, and limits the count to one. Public object
allocation, give/take, timeout, flush, and deletion are not implemented.

Task creation and registration remain unavailable, so task identity reports
undefined for external Zephyr threads, including the initial thread.
Other missing API providers remain link-time capability gaps. Initialization
success must not be interpreted as complete public API support. The focused
Zephyr initialization tests are in `osal-zephyr`'s `src/tests/zephyr-init`.
