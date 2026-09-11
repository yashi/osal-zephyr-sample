/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "harness.h"
#include "osapi.h"

#define FIRST_USEC  20000U
#define PERIOD_USEC 20000U
#define WATCHDOG_MS 2000
#define QUIET_MS    60

static struct k_sem callback_fired;
static atomic_t callback_count;
static osal_id_t callback_identity;

static void timer_callback(osal_id_t timer_id, void *arg)
{
    ARG_UNUSED(timer_id);
    ARG_UNUSED(arg);
    callback_identity = OS_TaskGetId();
    atomic_inc(&callback_count);
    k_sem_give(&callback_fired);
}

static bool wait_for_count(int expected)
{
    int64_t deadline = k_uptime_get() + WATCHDOG_MS;

    while (atomic_get(&callback_count) < expected &&
           k_uptime_get() < deadline) {
        (void)k_sem_take(&callback_fired, K_MSEC(20));
    }
    return atomic_get(&callback_count) >= expected;
}

int main(void)
{
    struct harness h;
    osal_id_t timebase;
    osal_id_t timer;
    osal_id_t found;
    OS_timebase_prop_t info;
    uint32 freerun_before;
    uint32 freerun_after;
    int stopped_count;
    int32 status;

    k_sem_init(&callback_fired, 0, 1);
    atomic_set(&callback_count, 0);
    callback_identity = OS_OBJECT_ID_UNDEFINED;
    harness_start(&h, "timebase");

    status = OS_API_Init();
    harness_check(&h, status == OS_SUCCESS, "initialize");
    if (status != OS_SUCCESS) {
        return harness_finish(&h);
    }

    status = OS_TimeBaseCreate(&timebase, "stage-timebase", NULL);
    harness_check(&h, status == OS_SUCCESS, "create-timebase");
    if (status != OS_SUCCESS) {
        return harness_finish(&h);
    }
    status = OS_TimerAdd(&timer, "stage-timer", timebase, timer_callback,
                         NULL);
    harness_check(&h, status == OS_SUCCESS, "add-timer");
    if (status != OS_SUCCESS) {
        (void)OS_TimeBaseDelete(timebase);
        return harness_finish(&h);
    }

    status = OS_TimerSet(timer, FIRST_USEC, PERIOD_USEC);
    harness_check(&h, status == OS_SUCCESS, "configure-timer");
    k_msleep(QUIET_MS);
    harness_check(&h, atomic_get(&callback_count) == 0,
                  "quiet-before-timebase-set");

    status = OS_TimeBaseGetFreeRun(timebase, &freerun_before);
    harness_check(&h, status == OS_SUCCESS && freerun_before == 0,
                  "initial-freerun");
    status = OS_TimeBaseSet(timebase, FIRST_USEC, PERIOD_USEC);
    harness_check(&h, status == OS_SUCCESS, "start-timebase");
    harness_check(&h, wait_for_count(2), "periodic-callbacks");
    harness_check(&h, OS_ObjectIdEqual(callback_identity, timebase),
                  "callback-identity");

    status = OS_TimeBaseGetFreeRun(timebase, &freerun_after);
    harness_check(&h, status == OS_SUCCESS &&
                      (uint32)(freerun_after - freerun_before) >=
                          FIRST_USEC + PERIOD_USEC,
                  "freerun-advances");
    status = OS_TimeBaseGetInfo(timebase, &info);
    harness_check(&h, status == OS_SUCCESS &&
                      strcmp(info.name, "stage-timebase") == 0 &&
                      info.nominal_interval_time == PERIOD_USEC,
                  "timebase-info");
    status = OS_TimeBaseGetIdByName(&found, "stage-timebase");
    harness_check(&h, status == OS_SUCCESS &&
                      OS_ObjectIdEqual(found, timebase),
                  "timebase-name");

    status = OS_TimeBaseSet(timebase, 0, PERIOD_USEC);
    stopped_count = atomic_get(&callback_count);
    k_msleep(QUIET_MS);
    harness_check(&h, status == OS_SUCCESS &&
                      atomic_get(&callback_count) == stopped_count,
                  "stop-timebase");

    status = OS_TimerSet(timer, PERIOD_USEC, 0);
    if (status == OS_SUCCESS) {
        status = OS_TimeBaseSet(timebase, PERIOD_USEC, 0);
    }
    harness_check(&h, status == OS_SUCCESS &&
                      wait_for_count(stopped_count + 1),
                  "one-shot-callback");
    stopped_count = atomic_get(&callback_count);
    k_msleep(QUIET_MS);
    harness_check(&h, atomic_get(&callback_count) == stopped_count,
                  "one-shot-stops");

    status = OS_TimerDelete(timer);
    harness_check(&h, status == OS_SUCCESS, "delete-timer");
    status = OS_TimeBaseDelete(timebase);
    harness_check(&h, status == OS_SUCCESS &&
                      OS_TimeBaseGetInfo(timebase, &info) == OS_ERR_INVALID_ID,
                  "delete-timebase-and-stale-id");

    return harness_finish(&h);
}
