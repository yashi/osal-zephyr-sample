/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "harness.h"
#include "osapi-clock.h"
#include "osapi-error.h"

#define TEST_DELAY_MS 20
#define TEST_SLOP_MS  100

static bool time_in_window(OS_time_t value, OS_time_t start, int32 window_ms)
{
    OS_time_t end = OS_TimeAdd(start, OS_TimeFromTotalMilliseconds(window_ms));

    return OS_TimeCompare(value, start) >= 0 && OS_TimeCompare(value, end) <= 0;
}

static bool monotonic_matches_uptime(OS_time_t *value)
{
    int64_t uptime_before_ms = k_uptime_get();
    int32   status           = OS_GetMonotonicTime(value);
    int64_t uptime_after_ms  = k_uptime_get();
    int64   monotonic_ms;

    if (status != OS_SUCCESS)
    {
        return false;
    }

    monotonic_ms = OS_TimeGetTotalMilliseconds(*value);

    return monotonic_ms >= uptime_before_ms && monotonic_ms <= uptime_after_ms;
}

int main(void)
{
    struct harness h;
    OS_time_t      monotonic_before;
    OS_time_t      monotonic_after;
    OS_time_t      local_before;
    OS_time_t      local_after;
    OS_time_t      target;

    harness_start(&h, "clock");

    harness_check(&h, OS_GetMonotonicTime(NULL) == OS_INVALID_POINTER,
                  "monotonic-null-pointer");
    harness_check(&h, OS_GetLocalTime(NULL) == OS_INVALID_POINTER,
                  "local-null-pointer");
    harness_check(&h, OS_SetLocalTime(NULL) == OS_INVALID_POINTER,
                  "set-null-pointer");

    harness_check(&h, monotonic_matches_uptime(&monotonic_before),
                  "get-monotonic-before");
    harness_check(&h, OS_GetLocalTime(&local_before) == OS_SUCCESS,
                  "get-local-before");
    harness_check(&h, time_in_window(local_before, monotonic_before, TEST_SLOP_MS),
                  "initial-local-uses-boot-epoch");
    k_sleep(K_MSEC(TEST_DELAY_MS));
    harness_check(&h, monotonic_matches_uptime(&monotonic_after),
                  "get-monotonic-after");
    harness_check(&h, OS_GetLocalTime(&local_after) == OS_SUCCESS,
                  "get-local-after");
    harness_check(&h, OS_TimeCompare(monotonic_after, monotonic_before) > 0,
                  "monotonic-advances");
    harness_check(&h, OS_TimeCompare(local_after, local_before) > 0,
                  "local-advances");

    target = OS_TimeAssembleFromMilliseconds(123, 456);
    harness_check(&h, OS_SetLocalTime(&target) == OS_SUCCESS,
                  "set-local-forward");
    harness_check(&h, OS_GetLocalTime(&local_after) == OS_SUCCESS,
                  "get-forward-time");
    harness_check(&h, time_in_window(local_after, target, TEST_SLOP_MS),
                  "forward-time-visible");

    target = OS_TimeAssembleFromMilliseconds(7, 890);
    harness_check(&h, OS_SetLocalTime(&target) == OS_SUCCESS,
                  "set-local-backward");
    harness_check(&h, OS_GetLocalTime(&local_after) == OS_SUCCESS,
                  "get-backward-time");
    harness_check(&h, time_in_window(local_after, target, TEST_SLOP_MS),
                  "backward-time-visible");

    harness_check(&h, monotonic_matches_uptime(&monotonic_after),
                  "get-monotonic-after-sets");
    harness_check(&h, OS_TimeCompare(monotonic_after, monotonic_before) > 0,
                  "local-sets-do-not-change-monotonic");

    target = OS_TIME_MAX;
    harness_check(&h, OS_SetLocalTime(&target) == OS_SUCCESS,
                  "set-local-maximum");
    k_sleep(K_MSEC(TEST_DELAY_MS));
    local_after = OS_TIME_MIN;
    harness_check(&h, OS_GetLocalTime(&local_after) == OS_ERROR,
                  "local-overflow-reported");
    harness_check(&h, OS_TimeEqual(local_after, OS_TIME_MIN),
                  "local-overflow-preserves-output");

    return harness_finish(&h);
}
