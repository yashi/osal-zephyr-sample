/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include <zephyr/ztest.h>

#include "osapi.h"

#define WATCHDOG_MS             3000
#define QUIET_MS                40
#define FIRST_DELAY_USEC        30000U
#define PERIOD_USEC             20000U
#define COALESCE_PERIOD_USEC    10000U
#define COALESCE_BLOCK_MS       120
#define TIMING_UPPER_MS         500
#define REUSE_ROUNDS            8
#define EXTERNAL_ELAPSED_TICKS  7U
#define SETTER_STACK_SIZE       1536

BUILD_ASSERT(OS_MAX_TIMEBASES == 2,
             "this fixture requires two timebase slots");
BUILD_ASSERT(OS_MAX_TIMERS == 3,
             "this fixture requires three timer slots");
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP),
             "this fixture requires UP scheduling");
BUILD_ASSERT(IS_ENABLED(CONFIG_SYS_CLOCK_EXISTS),
             "native timebases require a system clock");
BUILD_ASSERT(IS_ENABLED(CONFIG_THREAD_CUSTOM_DATA),
             "helper custom-data preservation is covered");

struct callback_probe
{
    struct k_sem fired;
    struct k_sem release;
    atomic_t count;
    atomic_t block_once;
    osal_id_t expected_timebase;
    osal_id_t observed_identity;
    int64_t first_tick;
    int64_t second_tick;
    int64_t last_tick;
    int custom_sentinel;
    bool custom_data_preserved;
    int32 timebase_set_status;
    int32 timebase_delete_status;
    int32 timer_set_status;
    int32 timer_delete_status;
    osal_id_t timer_id;
};

struct external_probe
{
    struct k_sem entered;
    struct k_sem release;
    atomic_t calls;
    atomic_t cooperative_stop;
    atomic_t elapsed;
    osal_id_t observed_identity;
    int custom_sentinel;
    bool custom_data_preserved;
};

struct setter_probe
{
    struct k_thread thread;
    struct k_sem started;
    struct k_sem done;
    osal_id_t timebase_id;
    int32 status;
};

static struct callback_probe callback_probe;
static struct external_probe external_probe;
static struct setter_probe setter_probe;
K_THREAD_STACK_DEFINE(setter_stack, SETTER_STACK_SIZE);
static struct k_sem dedicated_fired;
static atomic_t observe_heap_calls;
static atomic_t heap_alloc_calls;
static atomic_t heap_free_calls;
static atomic_t dedicated_callback_count;
static atomic_t fail_next_registration;

void *__real_k_malloc(size_t size);
void __real_k_free(void *ptr);
int32 __real_OS_TaskRegister_Impl(osal_id_t global_task_id);

void *__wrap_k_malloc(size_t size)
{
    if (atomic_get(&observe_heap_calls) != 0)
    {
        atomic_inc(&heap_alloc_calls);
    }
    return __real_k_malloc(size);
}

void __wrap_k_free(void *ptr)
{
    if (atomic_get(&observe_heap_calls) != 0)
    {
        atomic_inc(&heap_free_calls);
    }
    __real_k_free(ptr);
}

int32 __wrap_OS_TaskRegister_Impl(osal_id_t global_task_id)
{
    if (atomic_cas(&fail_next_registration, 1, 0))
    {
        return OS_ERROR;
    }
    return __real_OS_TaskRegister_Impl(global_task_id);
}

static void reset_callback_probe(void)
{
    k_sem_reset(&callback_probe.fired);
    k_sem_reset(&callback_probe.release);
    atomic_set(&callback_probe.count, 0);
    atomic_set(&callback_probe.block_once, 0);
    callback_probe.expected_timebase = OS_OBJECT_ID_UNDEFINED;
    callback_probe.observed_identity = OS_OBJECT_ID_UNDEFINED;
    callback_probe.first_tick = -1;
    callback_probe.second_tick = -1;
    callback_probe.last_tick = -1;
    callback_probe.custom_data_preserved = false;
    callback_probe.timebase_set_status = OS_ERROR;
    callback_probe.timebase_delete_status = OS_ERROR;
    callback_probe.timer_set_status = OS_ERROR;
    callback_probe.timer_delete_status = OS_ERROR;
    callback_probe.timer_id = OS_OBJECT_ID_UNDEFINED;
}

static void reset_external_probe(void)
{
    k_sem_reset(&external_probe.entered);
    k_sem_reset(&external_probe.release);
    atomic_set(&external_probe.calls, 0);
    atomic_set(&external_probe.cooperative_stop, 0);
    atomic_set(&external_probe.elapsed, 0);
    external_probe.observed_identity = OS_OBJECT_ID_UNDEFINED;
    external_probe.custom_data_preserved = false;
}

static void cooperatively_delete_external(osal_id_t timebase_id)
{
    int32 status = OS_ERROR;
    int64_t deadline = k_uptime_get() + WATCHDOG_MS;

    atomic_set(&external_probe.cooperative_stop, 1);
    k_sem_give(&external_probe.release);
    do
    {
        status = OS_TimeBaseDelete(timebase_id);
        if (status == OS_SUCCESS)
        {
            return;
        }
        zassert_equal(status, OS_ERROR,
                      "cooperative delete returned unexpected status");
        k_msleep(1);
    } while (k_uptime_get() < deadline);
    zassert_equal(status, OS_SUCCESS,
                  "cooperative external timebase delete did not complete");
}

static void timer_callback(osal_id_t timer_id, void *arg)
{
    struct callback_probe *probe = arg;
    int count = atomic_inc(&probe->count) + 1;

    ARG_UNUSED(timer_id);
    if (count == 1)
    {
        probe->first_tick = k_uptime_ticks();
    }
    else if (count == 2)
    {
        probe->second_tick = k_uptime_ticks();
    }
    probe->last_tick = k_uptime_ticks();
    k_sem_give(&probe->fired);
    if (atomic_cas(&probe->block_once, 1, 0))
    {
        (void)k_sem_take(&probe->release, K_FOREVER);
    }
}

static void dedicated_timer_callback(osal_id_t timer_id)
{
    ARG_UNUSED(timer_id);
    atomic_inc(&dedicated_callback_count);
    k_sem_give(&dedicated_fired);
}

static void setter_entry(void *arg1, void *arg2, void *arg3)
{
    struct setter_probe *probe = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    k_sem_give(&probe->started);
    probe->status = OS_TimeBaseSet(probe->timebase_id, 0, 0);
    k_sem_give(&probe->done);
}

static void context_callback(osal_id_t timer_id, void *arg)
{
    struct callback_probe *probe = arg;
    int count = atomic_inc(&probe->count) + 1;

    probe->observed_identity = OS_TaskGetId();
    if (OS_ObjectIdEqual(probe->observed_identity, probe->expected_timebase))
    {
        if (count == 1)
        {
            k_thread_custom_data_set(&probe->custom_sentinel);
            probe->timebase_set_status =
                OS_TimeBaseSet(probe->expected_timebase, 0, 0);
            probe->timebase_delete_status =
                OS_TimeBaseDelete(probe->expected_timebase);
            probe->timer_set_status = OS_TimerSet(timer_id, 1000, 0);
            probe->timer_delete_status = OS_TimerDelete(timer_id);
        }
        else
        {
            probe->custom_data_preserved =
                k_thread_custom_data_get() == &probe->custom_sentinel;
        }
    }
    k_sem_give(&probe->fired);
}

static uint32 external_sync(osal_id_t timebase_id)
{
    uint32 elapsed;
    int call = atomic_inc(&external_probe.calls) + 1;

    external_probe.observed_identity = OS_TaskGetId();
    if (call == 1)
    {
        k_thread_custom_data_set(&external_probe.custom_sentinel);
    }
    else
    {
        external_probe.custom_data_preserved =
            k_thread_custom_data_get() == &external_probe.custom_sentinel;
    }
    k_sem_give(&external_probe.entered);

    if (atomic_get(&external_probe.cooperative_stop) == 0)
    {
        (void)k_sem_take(&external_probe.release, K_FOREVER);
    }
    else
    {
        k_yield();
    }

    elapsed = (uint32)atomic_set(&external_probe.elapsed, 0);
    ARG_UNUSED(timebase_id);
    return elapsed;
}

static void create_timebase(osal_id_t *timebase_id, const char *name,
                            OS_TimerSync_t sync)
{
    int32 status = OS_TimeBaseCreate(timebase_id, name, sync);

    zassert_equal(status, OS_SUCCESS, "TimeBaseCreate %s returned %d", name,
                  (int)status);
}

static void add_timer(osal_id_t *timer_id, const char *name,
                      osal_id_t timebase_id, OS_ArgCallback_t callback)
{
    int32 status = OS_TimerAdd(timer_id, name, timebase_id, callback,
                               &callback_probe);

    zassert_equal(status, OS_SUCCESS, "TimerAdd %s returned %d", name,
                  (int)status);
}

static void wait_for_callbacks(int expected)
{
    int64_t deadline = k_uptime_get() + WATCHDOG_MS;

    while (atomic_get(&callback_probe.count) < expected &&
           k_uptime_get() < deadline)
    {
        (void)k_sem_take(&callback_probe.fired, K_MSEC(20));
    }
    zassert_true(atomic_get(&callback_probe.count) >= expected,
                 "received %d callbacks, expected at least %d",
                 (int)atomic_get(&callback_probe.count), expected);
}

static void wait_for_freerun(osal_id_t timebase_id, uint32 baseline,
                             uint32 minimum_delta)
{
    int64_t deadline = k_uptime_get() + WATCHDOG_MS;
    uint32 value = baseline;

    do
    {
        zassert_equal(OS_TimeBaseGetFreeRun(timebase_id, &value), OS_SUCCESS);
        if ((uint32)(value - baseline) >= minimum_delta)
        {
            return;
        }
        k_msleep(1);
    } while (k_uptime_get() < deadline);

    zassert_true(false, "freerun advanced %u, expected at least %u",
                 (unsigned int)(value - baseline),
                 (unsigned int)minimum_delta);
}

static int slot_for_id(osal_id_t id)
{
    osal_index_t index;

    zassert_equal(OS_ConvertToArrayIndex(id, &index), OS_SUCCESS);
    zassert_true(index < OS_MAX_TIMEBASES);
    return (int)index;
}

static void assert_tick_delay_in_bounds(int64_t actual_ticks,
                                        uint32 requested_usec)
{
    uint64_t actual_usec = k_ticks_to_us_floor64(actual_ticks);
    uint64_t tick_usec = k_ticks_to_us_ceil64(1);
    uint64_t lower_usec = requested_usec > 2 * tick_usec
                              ? requested_usec - 2 * tick_usec
                              : 0;

    zassert_true(actual_usec >= lower_usec,
                 "delay %llu us was below %llu us",
                 (unsigned long long)actual_usec,
                 (unsigned long long)lower_usec);
    zassert_true(actual_usec <= TIMING_UPPER_MS * 1000U,
                 "delay %llu us exceeded scheduling bound",
                 (unsigned long long)actual_usec);
}

ZTEST(osal_zephyr_timebase, test_public_limits_names_info_and_stale_ids)
{
    osal_id_t timebase[2];
    osal_id_t timer[3];
    osal_id_t found;
    osal_id_t extra;
    OS_timebase_prop_t timebase_info;
    OS_timer_prop_t timer_info;
    uint32 freerun;

    reset_callback_probe();
    zassert_equal(OS_TimeBaseCreate(NULL, "bad", NULL), OS_INVALID_POINTER);
    zassert_equal(OS_TimeBaseCreate(&extra, NULL, NULL), OS_INVALID_POINTER);
    create_timebase(&timebase[0], "limits-a", NULL);
    create_timebase(&timebase[1], "limits-b", NULL);
    zassert_equal(OS_TimeBaseCreate(&extra, "limits-c", NULL),
                  OS_ERR_NO_FREE_IDS);
    zassert_equal(OS_TimeBaseGetIdByName(&found, "limits-a"), OS_SUCCESS);
    zassert_true(OS_ObjectIdEqual(found, timebase[0]));
    zassert_equal(OS_TimeBaseGetInfo(timebase[0], &timebase_info), OS_SUCCESS);
    zassert_equal(strcmp(timebase_info.name, "limits-a"), 0);
    zassert_equal(OS_TimeBaseGetFreeRun(timebase[0], &freerun), OS_SUCCESS);
    zassert_equal(freerun, 0U);

    add_timer(&timer[0], "limits-t0", timebase[0], timer_callback);
    add_timer(&timer[1], "limits-t1", timebase[0], timer_callback);
    add_timer(&timer[2], "limits-t2", timebase[0], timer_callback);
    zassert_equal(OS_TimerAdd(&extra, "limits-t3", timebase[0],
                              timer_callback, &callback_probe),
                  OS_ERR_NO_FREE_IDS);
    zassert_equal(OS_TimerGetInfo(timer[0], &timer_info), OS_SUCCESS);
    zassert_equal(strcmp(timer_info.name, "limits-t0"), 0);

    for (size_t i = 0; i < ARRAY_SIZE(timer); ++i)
    {
        zassert_equal(OS_TimerDelete(timer[i]), OS_SUCCESS);
    }
    zassert_equal(OS_TimeBaseDelete(timebase[0]), OS_SUCCESS);
    zassert_equal(OS_TimeBaseGetInfo(timebase[0], &timebase_info),
                  OS_ERR_INVALID_ID);
    zassert_equal(OS_TimeBaseGetFreeRun(timebase[0], &freerun),
                  OS_ERR_INVALID_ID);
    zassert_equal(OS_TimeBaseSet(timebase[0], 1, 1), OS_ERR_INVALID_ID);
    zassert_equal(OS_TimeBaseDelete(timebase[0]), OS_ERR_INVALID_ID);
    zassert_equal(OS_TimerGetInfo(timer[0], &timer_info), OS_ERR_INVALID_ID);
    zassert_equal(OS_TimerSet(timer[0], 1, 0), OS_ERR_INVALID_ID);
    zassert_equal(OS_TimerDelete(timer[0]), OS_ERR_INVALID_ID);
    zassert_equal(OS_TimeBaseDelete(timebase[1]), OS_SUCCESS);
}

ZTEST(osal_zephyr_timebase, test_native_first_period_stop_and_oneshot)
{
    osal_id_t timebase;
    osal_id_t timer;
    int count;
    int64_t set_tick;
    uint32 freerun;
    OS_timebase_prop_t info;

    reset_callback_probe();
    create_timebase(&timebase, "native-sequence", NULL);
    add_timer(&timer, "native-timer", timebase, timer_callback);
    zassert_equal(OS_TimerSet(timer, FIRST_DELAY_USEC, PERIOD_USEC),
                  OS_SUCCESS);
    k_msleep(QUIET_MS);
    zassert_equal(atomic_get(&callback_probe.count), 0,
                  "callback ran before TimeBaseSet");

    set_tick = k_uptime_ticks();
    zassert_equal(OS_TimeBaseSet(timebase, FIRST_DELAY_USEC, PERIOD_USEC),
                  OS_SUCCESS);
    wait_for_callbacks(2);
    zassert_true(callback_probe.first_tick > set_tick,
                 "first callback did not wait for a native tick");
    zassert_true(callback_probe.second_tick > callback_probe.first_tick,
                 "periodic callback did not follow the first callback");
    assert_tick_delay_in_bounds(callback_probe.first_tick - set_tick,
                                FIRST_DELAY_USEC);
    assert_tick_delay_in_bounds(callback_probe.second_tick -
                                    callback_probe.first_tick,
                                PERIOD_USEC);
    zassert_equal(OS_TimeBaseGetInfo(timebase, &info), OS_SUCCESS);
    zassert_equal(info.nominal_interval_time, PERIOD_USEC);
    zassert_equal(info.accuracy, k_ticks_to_us_ceil32(1));
    zassert_equal(OS_TimeBaseGetFreeRun(timebase, &freerun), OS_SUCCESS);
    zassert_true(freerun >= FIRST_DELAY_USEC + PERIOD_USEC,
                 "freerun did not include first and periodic delays");

    zassert_equal(OS_TimeBaseSet(timebase, 0, PERIOD_USEC), OS_SUCCESS);
    count = atomic_get(&callback_probe.count);
    k_msleep(QUIET_MS + PERIOD_USEC / 1000U);
    zassert_equal(atomic_get(&callback_probe.count), count,
                  "start zero did not stop the source");

    zassert_equal(OS_TimerSet(timer, 1, 0), OS_SUCCESS);
    set_tick = k_uptime_ticks();
    zassert_equal(OS_TimeBaseSet(timebase, 1, 0), OS_SUCCESS);
    wait_for_callbacks(count + 1);
    zassert_true(callback_probe.last_tick > set_tick,
                 "one-microsecond request expired in the programming tick");
    count = atomic_get(&callback_probe.count);
    k_msleep(QUIET_MS);
    zassert_equal(atomic_get(&callback_probe.count), count,
                  "one-shot source produced another callback");

    zassert_equal(OS_TimerSet(timer, 1, 0), OS_SUCCESS);
    zassert_equal(OS_TimeBaseSet(timebase, 1, 0), OS_SUCCESS);
    wait_for_callbacks(count + 1);
    count = atomic_get(&callback_probe.count);
    k_msleep(QUIET_MS);
    zassert_equal(atomic_get(&callback_probe.count), count,
                  "repeated one-shot source did not stop");
    zassert_equal(OS_TimerDelete(timer), OS_SUCCESS);
    zassert_equal(OS_TimeBaseDelete(timebase), OS_SUCCESS);
}

ZTEST(osal_zephyr_timebase, test_native_coalesced_expiries_preserve_elapsed)
{
    osal_id_t timebase;
    osal_id_t timer;
    uint32 blocked_freerun;

    reset_callback_probe();
    atomic_set(&callback_probe.block_once, 1);
    create_timebase(&timebase, "coalesced", NULL);
    add_timer(&timer, "coalesced-timer", timebase, timer_callback);
    zassert_equal(OS_TimerSet(timer, COALESCE_PERIOD_USEC,
                              COALESCE_PERIOD_USEC), OS_SUCCESS);
    zassert_equal(OS_TimeBaseSet(timebase, COALESCE_PERIOD_USEC,
                                 COALESCE_PERIOD_USEC), OS_SUCCESS);
    wait_for_callbacks(1);
    zassert_equal(OS_TimeBaseGetFreeRun(timebase, &blocked_freerun),
                  OS_SUCCESS);
    k_msleep(COALESCE_BLOCK_MS);
    k_sem_give(&callback_probe.release);
    wait_for_freerun(timebase, blocked_freerun,
                     (COALESCE_BLOCK_MS - 40) * 1000U);

    zassert_equal(OS_TimeBaseSet(timebase, 0, 0), OS_SUCCESS);
    zassert_equal(OS_TimerDelete(timer), OS_SUCCESS);
    zassert_equal(OS_TimeBaseDelete(timebase), OS_SUCCESS);
}

ZTEST(osal_zephyr_timebase, test_stop_waits_for_active_callback_handoff)
{
    osal_id_t timebase;
    osal_id_t timer;
    int stopped_count;

    reset_callback_probe();
    atomic_set(&callback_probe.block_once, 1);
    k_sem_reset(&setter_probe.started);
    k_sem_reset(&setter_probe.done);
    setter_probe.status = OS_ERROR;
    create_timebase(&timebase, "blocked-stop", NULL);
    add_timer(&timer, "blocked-stop-t", timebase, timer_callback);
    zassert_equal(OS_TimerSet(timer, COALESCE_PERIOD_USEC,
                              COALESCE_PERIOD_USEC), OS_SUCCESS);
    zassert_equal(OS_TimeBaseSet(timebase, COALESCE_PERIOD_USEC,
                                 COALESCE_PERIOD_USEC), OS_SUCCESS);
    wait_for_callbacks(1);

    setter_probe.timebase_id = timebase;
    zassert_not_null(k_thread_create(
        &setter_probe.thread, setter_stack,
        K_THREAD_STACK_SIZEOF(setter_stack), setter_entry, &setter_probe,
        NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT));
    zassert_equal(k_sem_take(&setter_probe.started, K_MSEC(WATCHDOG_MS)), 0,
                  "setter did not start");
    zassert_not_equal(k_sem_take(&setter_probe.done, K_MSEC(30)), 0,
                      "Set completed while callback held the timebase");

    k_sem_give(&callback_probe.release);
    zassert_equal(k_sem_take(&setter_probe.done, K_MSEC(WATCHDOG_MS)), 0,
                  "Set did not complete after callback release");
    zassert_equal(k_thread_join(&setter_probe.thread, K_MSEC(WATCHDOG_MS)), 0,
                  "setter did not terminate");
    zassert_equal(setter_probe.status, OS_SUCCESS);
    stopped_count = atomic_get(&callback_probe.count);
    k_msleep(QUIET_MS);
    zassert_equal(atomic_get(&callback_probe.count), stopped_count,
                  "callback ran after contended stop completed");
    zassert_equal(OS_TimerDelete(timer), OS_SUCCESS);
    zassert_equal(OS_TimeBaseDelete(timebase), OS_SUCCESS);
}

ZTEST(osal_zephyr_timebase, test_failed_delete_with_timer_keeps_helper_live)
{
    osal_id_t timebase;
    osal_id_t timer;
    uint32 before;
    int count;
    int32 delete_status;

    reset_callback_probe();
    create_timebase(&timebase, "delete-rollback", NULL);
    add_timer(&timer, "delete-rollback-t", timebase, timer_callback);
    zassert_equal(OS_TimerSet(timer, PERIOD_USEC, PERIOD_USEC), OS_SUCCESS);
    zassert_equal(OS_TimeBaseSet(timebase, PERIOD_USEC, PERIOD_USEC),
                  OS_SUCCESS);
    wait_for_callbacks(1);
    zassert_equal(OS_TimeBaseGetFreeRun(timebase, &before), OS_SUCCESS);
    delete_status = OS_TimeBaseDelete(timebase);
    count = atomic_get(&callback_probe.count);
    wait_for_callbacks(count + 1);
    wait_for_freerun(timebase, before, PERIOD_USEC);
    zassert_equal(OS_TimerDelete(timer), OS_SUCCESS);
    zassert_equal(OS_TimeBaseDelete(timebase), OS_SUCCESS);
    zassert_equal(delete_status, OS_ERR_OBJECT_IN_USE,
                  "attached-timer delete returned %d", (int)delete_status);
}

ZTEST(osal_zephyr_timebase, test_callback_identity_context_and_custom_data)
{
    osal_id_t timebase;
    osal_id_t timer;

    reset_callback_probe();
    create_timebase(&timebase, "callback-context", NULL);
    callback_probe.expected_timebase = timebase;
    add_timer(&timer, "callback-context-t", timebase, context_callback);
    callback_probe.timer_id = timer;
    zassert_equal(OS_TimerSet(timer, PERIOD_USEC, PERIOD_USEC), OS_SUCCESS);
    zassert_equal(OS_TimeBaseSet(timebase, PERIOD_USEC, PERIOD_USEC),
                  OS_SUCCESS);
    wait_for_callbacks(2);

    zassert_true(OS_ObjectIdEqual(callback_probe.observed_identity, timebase),
                 "helper identity was not the owning timebase");
    zassert_equal(OS_IdentifyObject(callback_probe.observed_identity),
                  OS_OBJECT_TYPE_OS_TIMEBASE);
    zassert_true(callback_probe.custom_data_preserved,
                 "helper identity clobbered application custom data");
    zassert_equal(callback_probe.timebase_set_status,
                  OS_ERR_INCORRECT_OBJ_STATE);
    zassert_equal(callback_probe.timebase_delete_status,
                  OS_ERR_INCORRECT_OBJ_STATE);
    zassert_equal(callback_probe.timer_set_status,
                  OS_ERR_INCORRECT_OBJ_STATE);
    zassert_equal(callback_probe.timer_delete_status,
                  OS_ERR_INCORRECT_OBJ_STATE);
    zassert_equal(OS_TimeBaseSet(timebase, 0, 0), OS_SUCCESS);
    zassert_equal(OS_TimerDelete(timer), OS_SUCCESS);
    zassert_equal(OS_TimeBaseDelete(timebase), OS_SUCCESS);
}

ZTEST(osal_zephyr_timebase, test_static_reclamation_reuse_and_no_heap)
{
    osal_id_t old_id = OS_OBJECT_ID_UNDEFINED;
    osal_id_t id;
    osal_id_t timer;
    osal_id_t dedicated_timer;
    uint32 accuracy;
    int reused_slot = -1;
    atomic_val_t alloc_before = atomic_get(&heap_alloc_calls);
    atomic_val_t free_before = atomic_get(&heap_free_calls);

    reset_callback_probe();
    for (int i = 0; i < REUSE_ROUNDS; ++i)
    {
        create_timebase(&id, "reuse", NULL);
        add_timer(&timer, "reuse-timer", id, timer_callback);
        if (OS_ObjectIdDefined(old_id) && slot_for_id(id) == slot_for_id(old_id))
        {
            reused_slot = slot_for_id(id);
            zassert_not_equal(OS_ObjectIdToInteger(id),
                              OS_ObjectIdToInteger(old_id));
        }
        zassert_equal(OS_TimerDelete(timer), OS_SUCCESS);
        zassert_equal(OS_TimeBaseDelete(id), OS_SUCCESS);
        k_sem_reset(&dedicated_fired);
        zassert_equal(OS_TimerCreate(&dedicated_timer, "dedicated", &accuracy,
                                     dedicated_timer_callback), OS_SUCCESS);
        zassert_true(accuracy > 0);
        zassert_equal(OS_TimerSet(dedicated_timer, 5000, 5000), OS_SUCCESS);
        zassert_equal(k_sem_take(&dedicated_fired, K_MSEC(WATCHDOG_MS)), 0,
                      "armed dedicated timer did not fire");
        zassert_equal(OS_TimerDelete(dedicated_timer), OS_SUCCESS);
        old_id = id;
    }

    zassert_true(reused_slot >= 0, "no static timebase slot was reused");
    zassert_equal(atomic_get(&heap_alloc_calls), alloc_before,
                  "static timebase lifecycle called k_malloc");
    zassert_equal(atomic_get(&heap_free_calls), free_before,
                  "static timebase lifecycle called k_free");
}

ZTEST(osal_zephyr_timebase, test_registration_failure_unwinds_and_recovers)
{
    osal_id_t timebase;

    atomic_set(&fail_next_registration, 1);
    zassert_equal(OS_TimeBaseCreate(&timebase, "register-failure", NULL),
                  OS_TIMER_ERR_INTERNAL);
    zassert_equal(atomic_get(&fail_next_registration), 0,
                  "registration failure was not injected");
    create_timebase(&timebase, "register-failure", NULL);
    zassert_equal(OS_TimeBaseDelete(timebase), OS_SUCCESS);
}

ZTEST(osal_zephyr_timebase, test_external_uint32_max_elapsed_is_exact)
{
    osal_id_t timebase;
    uint32 freerun;

    reset_external_probe();
    create_timebase(&timebase, "external-max", external_sync);
    zassert_equal(k_sem_take(&external_probe.entered, K_MSEC(WATCHDOG_MS)), 0,
                  "external max sync was not entered");
    atomic_set(&external_probe.elapsed, (atomic_val_t)UINT32_MAX);
    k_sem_give(&external_probe.release);
    zassert_equal(k_sem_take(&external_probe.entered, K_MSEC(WATCHDOG_MS)), 0,
                  "large external elapsed value was not fully drained");
    zassert_equal(OS_TimeBaseGetFreeRun(timebase, &freerun), OS_SUCCESS);
    zassert_equal(freerun, UINT32_MAX,
                  "large elapsed time was dropped or wrapped early");
    cooperatively_delete_external(timebase);
}

ZTEST(osal_zephyr_timebase, test_zzz_external_sync_busy_delete_and_release)
{
    osal_id_t timebase;
    osal_id_t timer;
    OS_timebase_prop_t info;

    reset_callback_probe();
    reset_external_probe();
    create_timebase(&timebase, "external", external_sync);
    zassert_equal(k_sem_take(&external_probe.entered, K_MSEC(WATCHDOG_MS)), 0,
                  "external sync was not entered");
    zassert_true(OS_ObjectIdEqual(external_probe.observed_identity, timebase));

    zassert_equal(OS_TimeBaseSet(timebase, 123, 456), OS_SUCCESS);
    k_msleep(QUIET_MS);
    zassert_equal(atomic_get(&callback_probe.count), 0,
                  "TimeBaseSet drove an external source");
    zassert_equal(atomic_get(&external_probe.calls), 1,
                  "TimeBaseSet restarted the external sync function");

    zassert_equal(OS_TimeBaseDelete(timebase), OS_ERROR);
    zassert_equal(OS_TimeBaseGetInfo(timebase, &info), OS_SUCCESS);
    zassert_equal(strcmp(info.name, "external"), 0,
                  "failed delete did not preserve the timebase");

    add_timer(&timer, "external-timer", timebase, timer_callback);
    zassert_equal(OS_TimerSet(timer, EXTERNAL_ELAPSED_TICKS, 0), OS_SUCCESS);
    atomic_set(&external_probe.elapsed, EXTERNAL_ELAPSED_TICKS);
    k_sem_give(&external_probe.release);
    wait_for_callbacks(1);
    zassert_equal(k_sem_take(&external_probe.entered, K_MSEC(WATCHDOG_MS)), 0,
                  "external sync did not await its next event");
    zassert_true(external_probe.custom_data_preserved,
                 "external helper custom data did not survive an event");
    zassert_equal(OS_TimerDelete(timer), OS_SUCCESS);

    cooperatively_delete_external(timebase);
}

static void *suite_setup(void)
{
    k_sem_init(&callback_probe.fired, 0, 1);
    k_sem_init(&callback_probe.release, 0, 1);
    k_sem_init(&external_probe.entered, 0, 1);
    k_sem_init(&external_probe.release, 0, 1);
    k_sem_init(&setter_probe.started, 0, 1);
    k_sem_init(&setter_probe.done, 0, 1);
    k_sem_init(&dedicated_fired, 0, 1);
    atomic_set(&heap_alloc_calls, 0);
    atomic_set(&heap_free_calls, 0);
    atomic_set(&dedicated_callback_count, 0);
    atomic_set(&fail_next_registration, 0);
    atomic_set(&observe_heap_calls, 1);
    zassert_equal(OS_API_Init(), OS_SUCCESS);
    zassert_equal(atomic_get(&heap_alloc_calls), 0,
                  "OS_API_Init called k_malloc");
    zassert_equal(atomic_get(&heap_free_calls), 0,
                  "OS_API_Init called k_free");
    return NULL;
}

ZTEST_SUITE(osal_zephyr_timebase, NULL, suite_setup, NULL, NULL, NULL);
