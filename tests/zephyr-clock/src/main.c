/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "osapi-clock.h"
#include "osapi-error.h"

#define TEST_DELAY_MS 20
#define TEST_SLOP_MS  100
#define CONCURRENCY_ITERATIONS 1000
#define WORKER_STACK_SIZE      1024
#define WATCHDOG_MS            2000

static K_THREAD_STACK_DEFINE(setter_stack, WORKER_STACK_SIZE);
static struct k_thread setter_thread;
K_SEM_DEFINE(setter_done, 0, 1);
static atomic_t setter_failures;
static atomic_t setter_progress;

static const OS_time_t concurrent_targets[] = {
    { .ticks = 10001230000LL },
    { .ticks = 10000004560000LL },
};

static void assert_time_in_window(OS_time_t value, OS_time_t start)
{
    OS_time_t end = OS_TimeAdd(start, OS_TimeFromTotalMilliseconds(TEST_SLOP_MS));

    zassert_true(OS_TimeCompare(value, start) >= 0, "clock preceded set value");
    zassert_true(OS_TimeCompare(value, end) <= 0, "clock exceeded test window");
}

static bool time_near_target(OS_time_t value, OS_time_t target)
{
    OS_time_t end = OS_TimeAdd(target, OS_TimeFromTotalMilliseconds(TEST_SLOP_MS));

    return OS_TimeCompare(value, target) >= 0 && OS_TimeCompare(value, end) <= 0;
}

static void assert_monotonic_matches_uptime(OS_time_t *value)
{
    int64_t uptime_before_ms = k_uptime_get();
    int32   status           = OS_GetMonotonicTime(value);
    int64_t uptime_after_ms  = k_uptime_get();
    int64   monotonic_ms;

    zassert_equal(status, OS_SUCCESS);
    if (status != OS_SUCCESS)
    {
        return;
    }

    monotonic_ms = OS_TimeGetTotalMilliseconds(*value);
    zassert_true(monotonic_ms >= uptime_before_ms,
                 "monotonic time preceded Zephyr uptime");
    zassert_true(monotonic_ms <= uptime_after_ms,
                 "monotonic time exceeded Zephyr uptime");
}

static void concurrent_setter(void *arg1, void *arg2, void *arg3)
{
    unsigned int i;

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    for (i = 0; i < CONCURRENCY_ITERATIONS; ++i)
    {
        if (OS_SetLocalTime(&concurrent_targets[i % ARRAY_SIZE(concurrent_targets)]) != OS_SUCCESS)
        {
            atomic_inc(&setter_failures);
        }
        atomic_inc(&setter_progress);
        k_yield();
    }

    k_sem_give(&setter_done);
}

ZTEST(osal_zephyr_clock, test_public_errors_and_progress)
{
    OS_time_t monotonic_before;
    OS_time_t monotonic_after;
    OS_time_t local_before;
    OS_time_t local_after;

    zassert_equal(OS_GetMonotonicTime(NULL), OS_INVALID_POINTER);
    zassert_equal(OS_GetLocalTime(NULL), OS_INVALID_POINTER);
    zassert_equal(OS_SetLocalTime(NULL), OS_INVALID_POINTER);

    assert_monotonic_matches_uptime(&monotonic_before);
    zassert_equal(OS_GetLocalTime(&local_before), OS_SUCCESS);
    k_sleep(K_MSEC(TEST_DELAY_MS));
    assert_monotonic_matches_uptime(&monotonic_after);
    zassert_equal(OS_GetLocalTime(&local_after), OS_SUCCESS);
    zassert_true(OS_TimeCompare(monotonic_after, monotonic_before) > 0);
    zassert_true(OS_TimeCompare(local_after, local_before) > 0);
}

ZTEST(osal_zephyr_clock, test_forward_and_backward_changes)
{
    OS_time_t monotonic_before;
    OS_time_t monotonic_after;
    OS_time_t local;
    OS_time_t target = OS_TimeAssembleFromMilliseconds(123, 456);

    assert_monotonic_matches_uptime(&monotonic_before);
    zassert_equal(OS_SetLocalTime(&target), OS_SUCCESS);
    zassert_equal(OS_GetLocalTime(&local), OS_SUCCESS);
    assert_time_in_window(local, target);

    target = OS_TimeAssembleFromMilliseconds(7, 890);
    zassert_equal(OS_SetLocalTime(&target), OS_SUCCESS);
    zassert_equal(OS_GetLocalTime(&local), OS_SUCCESS);
    assert_time_in_window(local, target);

    assert_monotonic_matches_uptime(&monotonic_after);
    zassert_true(OS_TimeCompare(monotonic_after, monotonic_before) >= 0,
                 "setting local time changed monotonic time");
}

ZTEST(osal_zephyr_clock, test_local_time_overflow)
{
    OS_time_t local = OS_TIME_MIN;

    zassert_equal(OS_SetLocalTime(&OS_TIME_MAX), OS_SUCCESS);
    k_sleep(K_MSEC(TEST_DELAY_MS));
    zassert_equal(OS_GetLocalTime(&local), OS_ERROR);
    zassert_true(OS_TimeEqual(local, OS_TIME_MIN),
                 "failed read changed the output value");
}

ZTEST(osal_zephyr_clock, test_concurrent_set_and_get)
{
    OS_time_t    local;
    unsigned int i;
    bool         values_valid     = true;
    bool         overlap_observed = false;
    bool         worker_done;
    bool         worker_joined;

    k_sem_reset(&setter_done);
    atomic_set(&setter_failures, 0);
    atomic_set(&setter_progress, 0);
    zassert_equal(OS_SetLocalTime(&concurrent_targets[0]), OS_SUCCESS);
    zassert_not_null(
        k_thread_create(&setter_thread, setter_stack,
                        K_THREAD_STACK_SIZEOF(setter_stack), concurrent_setter,
                        NULL, NULL, NULL, k_thread_priority_get(k_current_get()),
                        0, K_NO_WAIT));

    for (i = 0; i < CONCURRENCY_ITERATIONS; ++i)
    {
        if (OS_GetLocalTime(&local) != OS_SUCCESS ||
            (!time_near_target(local, concurrent_targets[0]) &&
             !time_near_target(local, concurrent_targets[1])))
        {
            values_valid = false;
            break;
        }
        k_yield();
        overlap_observed = atomic_get(&setter_progress) != 0;
    }

    worker_done = k_sem_take(&setter_done, K_MSEC(WATCHDOG_MS)) == 0;
    if (!worker_done)
    {
        k_thread_abort(&setter_thread);
    }
    worker_joined = k_thread_join(&setter_thread, K_MSEC(WATCHDOG_MS)) == 0;

    zassert_true(worker_done, "concurrent setter did not finish");
    zassert_true(worker_joined, "concurrent setter did not join");
    zassert_equal(atomic_get(&setter_failures), 0,
                  "concurrent setter call failed");
    zassert_true(overlap_observed, "reader and setter did not overlap");
    zassert_true(values_valid, "reader observed a mixed clock anchor");
}

static void clock_after(void *fixture)
{
    OS_time_t zero = OS_TIME_ZERO;

    ARG_UNUSED(fixture);
    zassert_equal(OS_SetLocalTime(&zero), OS_SUCCESS);
}

ZTEST_SUITE(osal_zephyr_clock, NULL, NULL, NULL, clock_after, NULL);
