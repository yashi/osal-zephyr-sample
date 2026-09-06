/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/ztest.h>

#include "bsp-impl.h"
#include "os-shared-common.h"
#include "os-shared-console.h"
#include "os-shared-idmap.h"
#include "osapi-common.h"
#include "osapi-idmap.h"
#include "osapi-printf.h"
#include "osapi-task.h"

#define WORKER_STACK_SIZE 2048
#define WATCHDOG_MS       1000
#define EXCLUSION_MS      20
#define CAPTURE_SIZE      512

static K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK_SIZE);
static struct k_thread worker_thread;
static K_THREAD_STACK_DEFINE(writer_stack_a, WORKER_STACK_SIZE);
static K_THREAD_STACK_DEFINE(writer_stack_b, WORKER_STACK_SIZE);
static struct k_thread writer_thread_a;
static struct k_thread writer_thread_b;

K_SEM_DEFINE(worker_ready, 0, 1);
K_SEM_DEFINE(worker_acquired, 0, 1);
K_SEM_DEFINE(worker_done, 0, 1);
K_SEM_DEFINE(writers_ready, 0, 2);
K_SEM_DEFINE(writers_go, 0, 2);
K_SEM_DEFINE(writers_done, 0, 2);

static osal_id_t worker_task_id;
static bool      protected_predicate;
static int64_t   wake_start_ticks;
static int64_t   wake_acquired_ticks;

static uint8_t capture_buffer[CAPTURE_SIZE];
static size_t  capture_length;
static bool    capture_overflow;

static int capture_char(int c)
{
    if (capture_length < sizeof(capture_buffer))
    {
        capture_buffer[capture_length++] = (uint8_t)c;
    }
    else
    {
        capture_overflow = true;
    }

    return c;
}

static printk_hook_fn_t capture_begin(void)
{
    printk_hook_fn_t original_hook;

    capture_length   = 0;
    capture_overflow = false;
    original_hook    = __printk_get_hook();
    __printk_hook_install(capture_char);

    return original_hook;
}

static void capture_end(printk_hook_fn_t original_hook)
{
    __printk_hook_install(original_hook);
}

static void reset_worker_signals(void)
{
    k_sem_reset(&worker_ready);
    k_sem_reset(&worker_acquired);
    k_sem_reset(&worker_done);
}

static k_tid_t start_worker(k_thread_entry_t entry, void *arg)
{
    return k_thread_create(&worker_thread, worker_stack,
                           K_THREAD_STACK_SIZEOF(worker_stack), entry, arg,
                           NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
}

static bool join_finished_worker(void)
{
    if (k_sem_take(&worker_done, K_MSEC(WATCHDOG_MS)) != 0)
    {
        k_thread_abort(&worker_thread);
        (void)k_thread_join(&worker_thread, K_MSEC(WATCHDOG_MS));
        return false;
    }

    return k_thread_join(&worker_thread, K_MSEC(WATCHDOG_MS)) == 0;
}

static void task_id_worker(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    worker_task_id = OS_TaskGetId();
    k_sem_give(&worker_done);
}

static void bsp_lock_worker(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_give(&worker_ready);
    OS_BSP_Lock_Impl();
    k_sem_give(&worker_acquired);
    OS_BSP_Unlock_Impl();
    k_sem_give(&worker_done);
}

static void table_lock_worker(void *arg1, void *arg2, void *arg3)
{
    osal_objtype_t idtype = (osal_objtype_t)(uintptr_t)arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_give(&worker_ready);
    OS_Lock_Global_Impl(idtype);
    k_sem_give(&worker_acquired);
    OS_Unlock_Global_Impl(idtype);
    k_sem_give(&worker_done);
}

static void shared_lock_worker(void *arg1, void *arg2, void *arg3)
{
    osal_objtype_t    idtype = (osal_objtype_t)(uintptr_t)arg1;
    OS_object_token_t token;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_give(&worker_ready);
    if (OS_ObjectIdTransactionInit(OS_LOCK_MODE_GLOBAL, idtype, &token) == OS_SUCCESS)
    {
        k_sem_give(&worker_acquired);
        OS_ObjectIdTransactionCancel(&token);
    }
    k_sem_give(&worker_done);
}

static void predicate_waker(void *arg1, void *arg2, void *arg3)
{
    osal_objtype_t idtype = (osal_objtype_t)(uintptr_t)arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_give(&worker_ready);
    k_sleep(K_MSEC(EXCLUSION_MS));
    OS_Lock_Global_Impl(idtype);
    protected_predicate = true;
    wake_acquired_ticks = k_uptime_ticks() - wake_start_ticks;
    OS_Unlock_Global_Impl(idtype);
    k_sem_give(&worker_done);
}

static void idle_impl_worker(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_give(&worker_ready);
    OS_IdleLoop_Impl();
    k_sem_give(&worker_done);
}

static void idle_public_worker(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_give(&worker_ready);
    OS_IdleLoop();
    k_sem_give(&worker_done);
}

static void printf_writer(void *arg1, void *arg2, void *arg3)
{
    const char *message = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_give(&writers_ready);
    if (k_sem_take(&writers_go, K_MSEC(WATCHDOG_MS)) == 0)
    {
        OS_printf("%s", message);
    }
    k_sem_give(&writers_done);
}

static void *suite_setup(void)
{
    int32 status = OS_API_Init();

    zassert_equal(status, OS_SUCCESS, "OS_API_Init failed: %d", (int)status);
    return NULL;
}

ZTEST(osal_zephyr_init, test_api_init_and_repeated_init)
{
    osal_id_t console_id = OS_SharedGlobalVars.PrintfConsoleId;
    char     *buffer     = OS_console_table[0].BufBase;
    size_t    buffer_size = OS_console_table[0].BufSize;
    int32     status;

    zassert_equal(OS_SharedGlobalVars.GlobalState, OS_INIT_MAGIC_NUMBER,
                  "global state was not initialized");
    zassert_not_equal(OS_SharedGlobalVars.TicksPerSecond, 0,
                      "tick rate was not initialized");
    zassert_not_equal(OS_SharedGlobalVars.MicroSecPerTick, 0,
                      "tick period was not initialized");
    zassert_equal(OS_SharedGlobalVars.TicksPerSecond,
                  CONFIG_SYS_CLOCK_TICKS_PER_SEC, "wrong tick rate");
    zassert_equal(OS_SharedGlobalVars.MicroSecPerTick,
                  1000000U / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                  "wrong whole-microsecond tick period");
    zassert_true(OS_ObjectIdDefined(console_id), "printf console has no ID");
    zassert_not_null(buffer, "printf console has no buffer");
    zassert_false(OS_console_table[0].IsAsync,
                  "focused console unexpectedly selected async mode");

    status = OS_API_Init();
    zassert_equal(status, OS_SUCCESS, "repeated init failed: %d", (int)status);
    zassert_true(OS_ObjectIdEqual(OS_SharedGlobalVars.PrintfConsoleId, console_id),
                 "repeated init changed console ID");
    zassert_equal(OS_console_table[0].BufBase, buffer,
                  "repeated init changed console buffer");
    zassert_equal(OS_console_table[0].BufSize, buffer_size,
                  "repeated init changed console size");
}

ZTEST(osal_zephyr_init, test_external_thread_identity)
{
    bool joined;

    zassert_false(OS_ObjectIdDefined(OS_TaskGetId()),
                  "ztest thread unexpectedly has an OSAL task ID");

    reset_worker_signals();
    worker_task_id = OS_OBJECT_ID_RESERVED;
    zassert_not_null(start_worker(task_id_worker, NULL), "thread create failed");
    joined = join_finished_worker();

    zassert_true(joined, "task-ID worker did not finish");
    zassert_false(OS_ObjectIdDefined(worker_task_id),
                  "native worker unexpectedly has an OSAL task ID");
}

ZTEST(osal_zephyr_init, test_raw_bsp_exact_length)
{
    static const uint8_t raw[] = {'A', 0, 'B', 'C'};
    printk_hook_fn_t     original_hook;
    size_t               length;
    bool                 overflow;
    uint8_t              captured[sizeof(raw)];

    /* No assertions or worker threads may emit text until the hook is restored. */
    k_sched_lock();
    original_hook = capture_begin();
    OS_BSP_ConsoleOutput_Impl((const char *)raw, sizeof(raw));
    OS_BSP_ConsoleOutput_Impl((const char *)raw, 0);
    length   = capture_length;
    overflow = capture_overflow;
    memcpy(captured, capture_buffer, sizeof(captured));
    capture_end(original_hook);
    k_sched_unlock();

    zassert_false(overflow, "capture overflowed");
    zassert_equal(length, sizeof(raw), "raw BSP output length changed");
    zassert_mem_equal(captured, raw, sizeof(raw),
                      "raw BSP output changed byte content");
}

ZTEST(osal_zephyr_init, test_os_printf_wrap_and_controls)
{
    static const char expected[] = OS_PRINTF_CONSOLE_NAME "wrap:7"
                                   OS_PRINTF_CONSOLE_NAME "visible";
    OS_console_internal_record_t *console = &OS_console_table[0];
    printk_hook_fn_t              original_hook;
    size_t                        first_position;
    size_t                        final_position;
    size_t                        length;
    uint32                        overflow_events;
    bool                          overflow;
    bool                          first_drained;
    bool                          final_drained;
    uint8_t                       captured[sizeof(expected) - 1];

    zassert_true(console->BufSize > strlen(expected) + 2,
                 "console buffer is too small for wrap test");

    overflow_events   = console->OverflowEvents;
    console->ReadPos  = console->BufSize - 2;
    console->WritePos = console->ReadPos;

    /* No assertions or worker threads may emit text until the hook is restored. */
    k_sched_lock();
    original_hook = capture_begin();
    OS_printf_enable();
    OS_printf("wrap:%u", 7U);
    first_position = console->WritePos;
    first_drained  = console->ReadPos == console->WritePos;
    OS_printf_disable();
    OS_printf("hidden");
    OS_printf_enable();
    OS_printf("visible");
    final_position = console->WritePos;
    final_drained  = console->ReadPos == console->WritePos;
    length         = capture_length;
    overflow       = capture_overflow;
    memcpy(captured, capture_buffer, sizeof(captured));
    capture_end(original_hook);
    k_sched_unlock();

    zassert_false(overflow, "capture overflowed");
    zassert_equal(length, sizeof(expected) - 1, "OS_printf output length changed");
    zassert_mem_equal(captured, expected, sizeof(expected) - 1,
                      "OS_printf output content changed");
    zassert_true(first_drained, "first synchronous write was not drained");
    zassert_true(final_drained, "final synchronous write was not drained");
    zassert_equal(first_position,
                  (console->BufSize - 2 + strlen(OS_PRINTF_CONSOLE_NAME "wrap:7")) %
                      console->BufSize,
                  "first write did not wrap to the expected position");
    zassert_equal(final_position,
                  (first_position + strlen(OS_PRINTF_CONSOLE_NAME "visible")) %
                      console->BufSize,
                  "final write position is wrong");
    zassert_equal(console->OverflowEvents, overflow_events,
                  "synchronous writes overflowed");
    zassert_true(OS_SharedGlobalVars.PrintfEnabled,
                 "OS_printf was not re-enabled");
}

ZTEST(osal_zephyr_init, test_concurrent_os_printf_records)
{
    static const char message_a[] = "writer-A\n";
    static const char message_b[] = "writer-B\n";
    static const char record_a[]  = OS_PRINTF_CONSOLE_NAME "writer-A\n";
    static const char record_b[]  = OS_PRINTF_CONSOLE_NAME "writer-B\n";
    printk_hook_fn_t  original_hook;
    size_t            length;
    bool              overflow;
    bool              ready;
    bool              done;
    bool              joined;
    bool              records_intact;
    uint8_t           captured[(sizeof(record_a) - 1) + (sizeof(record_b) - 1)];

    k_sem_reset(&writers_ready);
    k_sem_reset(&writers_go);
    k_sem_reset(&writers_done);

    /* Hold the output lock until both writers have entered the operation. */
    OS_BSP_Lock_Impl();
    original_hook = capture_begin();
    k_thread_create(&writer_thread_a, writer_stack_a,
                    K_THREAD_STACK_SIZEOF(writer_stack_a), printf_writer,
                    (void *)message_a, NULL, NULL, K_PRIO_PREEMPT(0), 0,
                    K_NO_WAIT);
    k_thread_create(&writer_thread_b, writer_stack_b,
                    K_THREAD_STACK_SIZEOF(writer_stack_b), printf_writer,
                    (void *)message_b, NULL, NULL, K_PRIO_PREEMPT(0), 0,
                    K_NO_WAIT);
    ready = k_sem_take(&writers_ready, K_MSEC(WATCHDOG_MS)) == 0;
    ready = (k_sem_take(&writers_ready, K_MSEC(WATCHDOG_MS)) == 0) && ready;
    k_sem_give(&writers_go);
    k_sem_give(&writers_go);
    k_sleep(K_MSEC(EXCLUSION_MS));
    OS_BSP_Unlock_Impl();
    done = k_sem_take(&writers_done, K_MSEC(WATCHDOG_MS)) == 0;
    done = (k_sem_take(&writers_done, K_MSEC(WATCHDOG_MS)) == 0) && done;
    joined = k_thread_join(&writer_thread_a, K_MSEC(WATCHDOG_MS)) == 0;
    joined = (k_thread_join(&writer_thread_b, K_MSEC(WATCHDOG_MS)) == 0) && joined;
    length   = capture_length;
    overflow = capture_overflow;
    memcpy(captured, capture_buffer, sizeof(captured));
    capture_end(original_hook);

    records_intact =
        (memcmp(captured, record_a, sizeof(record_a) - 1) == 0 &&
         memcmp(captured + sizeof(record_a) - 1, record_b, sizeof(record_b) - 1) == 0) ||
        (memcmp(captured, record_b, sizeof(record_b) - 1) == 0 &&
         memcmp(captured + sizeof(record_b) - 1, record_a, sizeof(record_a) - 1) == 0);

    zassert_true(ready, "printf writers did not become ready");
    zassert_true(done, "printf writers did not finish");
    zassert_true(joined, "printf writers did not join");
    zassert_false(overflow, "concurrent capture overflowed");
    zassert_equal(length, sizeof(captured), "concurrent output length changed");
    zassert_true(records_intact, "concurrent OS_printf records interleaved");
}

ZTEST(osal_zephyr_init, test_async_console_is_rejected)
{
    OS_object_token_t token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_CONSOLE,
        .obj_idx   = 0,
    };
    bool  saved_async = OS_console_table[0].IsAsync;
    int32 status;

    OS_console_table[0].IsAsync = true;
    status = OS_ConsoleCreate_Impl(&token);
    OS_console_table[0].IsAsync = saved_async;

    zassert_equal(status, OS_ERR_NOT_IMPLEMENTED,
                  "async console creation returned %d", (int)status);
    zassert_false(OS_console_table[0].IsAsync,
                  "test did not restore synchronous state");
}

ZTEST(osal_zephyr_init, test_recursive_bsp_lock_exclusion)
{
    bool blocked_twice;
    bool blocked_once;
    bool joined;

    reset_worker_signals();
    OS_BSP_Lock_Impl();
    OS_BSP_Lock_Impl();
    start_worker(bsp_lock_worker, NULL);
    (void)k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS));
    blocked_twice = k_sem_take(&worker_acquired, K_MSEC(EXCLUSION_MS)) != 0;
    OS_BSP_Unlock_Impl();
    blocked_once = k_sem_take(&worker_acquired, K_MSEC(EXCLUSION_MS)) != 0;
    OS_BSP_Unlock_Impl();
    joined = join_finished_worker();

    zassert_true(blocked_twice, "worker entered doubly held BSP lock");
    zassert_true(blocked_once, "one unlock released recursive BSP lock");
    zassert_true(joined, "BSP lock worker did not finish");
}

ZTEST(osal_zephyr_init, test_all_backend_table_mutexes)
{
    for (osal_objtype_t idtype = OS_OBJECT_TYPE_UNDEFINED;
         idtype < OS_OBJECT_TYPE_USER; ++idtype)
    {
        bool blocked_twice;
        bool blocked_once;
        bool joined;

        reset_worker_signals();
        OS_Lock_Global_Impl(idtype);
        OS_Lock_Global_Impl(idtype);
        start_worker(table_lock_worker, (void *)(uintptr_t)idtype);
        (void)k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS));
        blocked_twice = k_sem_take(&worker_acquired, K_MSEC(EXCLUSION_MS)) != 0;
        OS_Unlock_Global_Impl(idtype);
        blocked_once = k_sem_take(&worker_acquired, K_MSEC(EXCLUSION_MS)) != 0;
        OS_Unlock_Global_Impl(idtype);
        joined = join_finished_worker();

        zassert_true(blocked_twice, "type %u ignored two lock levels", idtype);
        zassert_true(blocked_once, "type %u ignored one lock level", idtype);
        zassert_true(joined, "type %u worker did not finish", idtype);
    }
}

ZTEST(osal_zephyr_init, test_shared_state_change_wait)
{
    const osal_objtype_t idtype = OS_OBJECT_TYPE_OS_CONSOLE;
    OS_object_token_t    token;
    bool                 worker_entered;
    bool                 worker_locked;
    bool                 reacquired;
    bool                 joined;
    int64_t              start_ticks;
    int64_t              timeout_ticks;
    int32                status;

    status = OS_ObjectIdTransactionInit(OS_LOCK_MODE_GLOBAL, idtype, &token);
    zassert_equal(status, OS_SUCCESS, "main transaction failed: %d", (int)status);

    reset_worker_signals();
    start_worker(shared_lock_worker, (void *)(uintptr_t)idtype);
    worker_entered = k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS)) == 0;
    OS_WaitForStateChange(&token, 1);
    joined = join_finished_worker();
    worker_locked = k_sem_take(&worker_acquired, K_NO_WAIT) == 0;

    /* The wait must have reacquired the lock before returning. */
    reset_worker_signals();
    start_worker(table_lock_worker, (void *)(uintptr_t)idtype);
    (void)k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS));
    reacquired = k_sem_take(&worker_acquired, K_MSEC(EXCLUSION_MS)) != 0;
    OS_ObjectIdTransactionCancel(&token);
    joined = join_finished_worker() && joined;

    status = OS_ObjectIdTransactionInit(OS_LOCK_MODE_GLOBAL, idtype, &token);
    zassert_equal(status, OS_SUCCESS, "timeout transaction failed: %d", (int)status);
    start_ticks = k_uptime_ticks();
    OS_WaitForStateChange(&token, 1);
    timeout_ticks = k_uptime_ticks() - start_ticks;
    OS_ObjectIdTransactionCancel(&token);

    printk("OSAL wait timeout ticks: %lld\n", (long long)timeout_ticks);
    zassert_true(worker_entered, "state-change worker did not start");
    zassert_true(worker_locked, "state-change worker did not acquire the lock");
    zassert_true(reacquired, "state-change wait returned without owning lock");
    zassert_true(joined, "state-change workers did not finish");
    zassert_true(timeout_ticks > 0, "bounded timeout returned no later tick");
}

ZTEST(osal_zephyr_init, test_condvar_against_bounded_backoff)
{
    const osal_objtype_t idtype = OS_OBJECT_TYPE_OS_RWLOCK;

    for (uint32_t attempt = 1; attempt <= 4; ++attempt)
    {
        const uint32_t backoff_ms = 10U * (attempt + 1U) * (attempt + 1U);

        for (uint32_t trial = 1; trial <= 3; ++trial)
        {
            int64_t condvar_ticks;
            int64_t condvar_waker_ticks;
            int64_t backoff_ticks;
            int64_t backoff_waker_ticks;
            bool    condvar_predicate;
            bool    backoff_predicate;
            bool    condvar_joined;
            bool    backoff_joined;

            reset_worker_signals();
            protected_predicate = false;
            OS_Lock_Global_Impl(idtype);
            wake_start_ticks = k_uptime_ticks();
            start_worker(predicate_waker, (void *)(uintptr_t)idtype);
            (void)k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS));
            OS_WaitForStateChange_Impl(idtype, attempt);
            condvar_ticks       = k_uptime_ticks() - wake_start_ticks;
            condvar_waker_ticks = wake_acquired_ticks;
            condvar_predicate   = protected_predicate;
            OS_Unlock_Global_Impl(idtype);
            condvar_joined = join_finished_worker();

            reset_worker_signals();
            protected_predicate = false;
            OS_Lock_Global_Impl(idtype);
            wake_start_ticks = k_uptime_ticks();
            start_worker(predicate_waker, (void *)(uintptr_t)idtype);
            (void)k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS));
            OS_Unlock_Global_Impl(idtype);
            k_sleep(K_MSEC(backoff_ms));
            OS_Lock_Global_Impl(idtype);
            backoff_ticks       = k_uptime_ticks() - wake_start_ticks;
            backoff_waker_ticks = wake_acquired_ticks;
            backoff_predicate   = protected_predicate;
            OS_Unlock_Global_Impl(idtype);
            backoff_joined = join_finished_worker();

            printk("OSAL wait comparison: attempt=%u trial=%u "
                   "condvar=%lld waker=%lld backoff=%lld waker=%lld\n",
                   attempt, trial, (long long)condvar_ticks,
                   (long long)condvar_waker_ticks, (long long)backoff_ticks,
                   (long long)backoff_waker_ticks);
            zassert_true(condvar_predicate,
                         "attempt %u trial %u condvar predicate unchanged",
                         attempt, trial);
            zassert_true(backoff_predicate,
                         "attempt %u trial %u backoff predicate unchanged",
                         attempt, trial);
            zassert_true(condvar_joined,
                         "attempt %u trial %u condvar worker did not finish",
                         attempt, trial);
            zassert_true(backoff_joined,
                         "attempt %u trial %u backoff worker did not finish",
                         attempt, trial);
            zassert_true(condvar_waker_ticks <= condvar_ticks,
                         "attempt %u trial %u condvar returned before update",
                         attempt, trial);
            zassert_true(backoff_waker_ticks <= backoff_ticks,
                         "attempt %u trial %u backoff returned before update",
                         attempt, trial);
            zassert_true(condvar_ticks < backoff_ticks,
                         "attempt %u trial %u notification did not beat backoff",
                         attempt, trial);
        }
    }
}

ZTEST(osal_zephyr_init, test_idle_impl_latched_notification)
{
    bool joined_before;
    bool was_waiting;
    bool joined_during;

    OS_ApplicationShutdown_Impl();
    reset_worker_signals();
    start_worker(idle_impl_worker, NULL);
    joined_before = join_finished_worker();

    reset_worker_signals();
    start_worker(idle_impl_worker, NULL);
    (void)k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS));
    was_waiting = k_thread_join(&worker_thread, K_MSEC(EXCLUSION_MS)) != 0;
    OS_ApplicationShutdown_Impl();
    joined_during = join_finished_worker();

    zassert_true(joined_before, "pre-wait notification was lost");
    zassert_true(was_waiting, "idle worker did not block before notification");
    zassert_true(joined_during, "waiting idle worker did not wake");
}

ZTEST(osal_zephyr_init, test_public_shutdown_and_quiescent_reinit)
{
    bool  was_waiting;
    bool  joined;
    int32 status;

    reset_worker_signals();
    start_worker(idle_public_worker, NULL);
    (void)k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS));
    was_waiting = k_thread_join(&worker_thread, K_MSEC(EXCLUSION_MS)) != 0;
    OS_ApplicationShutdown(true);
    joined = join_finished_worker();

    /* Reinitialize only after the sole idle waiter has terminated. */
    status = OS_API_Init();

    zassert_true(was_waiting, "public idle loop did not wait");
    zassert_true(joined, "public shutdown did not release idle loop");
    zassert_equal(status, OS_SUCCESS, "quiescent reinit failed: %d", (int)status);
    zassert_equal(OS_SharedGlobalVars.GlobalState, OS_INIT_MAGIC_NUMBER,
                  "quiescent reinit did not restore initialized state");
}

ZTEST_SUITE(osal_zephyr_init, NULL, suite_setup, NULL, NULL, NULL);
