/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "os-shared-condvar.h"
#include "os-shared-idmap.h"

#define WORKER_COUNT      3
#define WORKER_STACK_SIZE 2048
#define WATCHDOG_MS       1000

static K_THREAD_STACK_DEFINE(worker_stack_0, WORKER_STACK_SIZE);
static K_THREAD_STACK_DEFINE(worker_stack_1, WORKER_STACK_SIZE);
static K_THREAD_STACK_DEFINE(worker_stack_2, WORKER_STACK_SIZE);
static struct k_thread worker_thread[WORKER_COUNT];
static struct k_sem    worker_ready;
static struct k_sem    worker_done;

static OS_object_token_t condvar_token = {
    .lock_mode = OS_LOCK_MODE_NONE,
    .obj_type  = OS_OBJECT_TYPE_OS_CONDVAR,
    .obj_idx   = 0,
};

static unsigned int permits;
static unsigned int wake_count;
static unsigned int wait_return_count;
static int32        worker_status[WORKER_COUNT];
static bool         workers_started;

static void condvar_worker(unsigned int index)
{
    int32 status;

    status = OS_CondVarLock_Impl(&condvar_token);
    k_sem_give(&worker_ready);

    while (status == OS_SUCCESS && permits == 0)
    {
        status = OS_CondVarWait_Impl(&condvar_token);
        if (status == OS_SUCCESS)
        {
            ++wait_return_count;
        }
    }

    if (status == OS_SUCCESS)
    {
        --permits;
        ++wake_count;
        status = OS_CondVarUnlock_Impl(&condvar_token);
    }

    worker_status[index] = status;
    k_sem_give(&worker_done);
}

static void worker_0(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    condvar_worker(0);
}

static void worker_1(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    condvar_worker(1);
}

static void worker_2(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    condvar_worker(2);
}

static void condvar_after(void *fixture)
{
    unsigned int i;

    ARG_UNUSED(fixture);

    if (workers_started)
    {
        for (i = 0; i < WORKER_COUNT; ++i)
        {
            k_thread_abort(&worker_thread[i]);
            (void)k_thread_join(&worker_thread[i], K_MSEC(WATCHDOG_MS));
        }
        workers_started = false;
    }

    /* A failed assertion may leave application ownership or a waiter
     * reservation behind. Do not hide that failure by resetting live
     * native storage; successful cases delete their own object. */
}

ZTEST(osal_zephyr_condvar, test_signal_and_broadcast)
{
    int32 status;
    bool  ready;
    bool  done;
    bool  joined;

    zassert_equal(OS_CondVarCreate_Impl(&condvar_token, 0), OS_SUCCESS);
    zassert_equal(OS_CondVarSignal_Impl(&condvar_token), OS_SUCCESS);

    permits           = 0;
    wake_count        = 0;
    wait_return_count = 0;
    worker_status[0]  = OS_ERROR;
    worker_status[1]  = OS_ERROR;
    worker_status[2]  = OS_ERROR;
    k_sem_init(&worker_ready, 0, WORKER_COUNT);
    k_sem_init(&worker_done, 0, WORKER_COUNT);

    k_thread_create(&worker_thread[0], worker_stack_0,
                    K_THREAD_STACK_SIZEOF(worker_stack_0), worker_0,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    k_thread_create(&worker_thread[1], worker_stack_1,
                    K_THREAD_STACK_SIZEOF(worker_stack_1), worker_1,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    k_thread_create(&worker_thread[2], worker_stack_2,
                    K_THREAD_STACK_SIZEOF(worker_stack_2), worker_2,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    workers_started = true;

    ready = k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS)) == 0;
    ready = (k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS)) == 0) && ready;
    ready = (k_sem_take(&worker_ready, K_MSEC(WATCHDOG_MS)) == 0) && ready;
    zassert_true(ready, "workers did not reach the condition wait");

    status = OS_CondVarLock_Impl(&condvar_token);
    zassert_equal(status, OS_SUCCESS);
    zassert_not_equal(k_sem_take(&worker_done, K_NO_WAIT), 0);
    zassert_equal(wait_return_count, 0,
                  "a notification was retained for a later waiter");

    permits = 1;
    zassert_equal(OS_CondVarSignal_Impl(&condvar_token), OS_SUCCESS);
    done = k_sem_take(&worker_done, K_MSEC(20)) == 0;
    zassert_false(done, "wait returned before reacquiring the mutex");
    zassert_equal(OS_CondVarUnlock_Impl(&condvar_token), OS_SUCCESS);

    done = k_sem_take(&worker_done, K_MSEC(WATCHDOG_MS)) == 0;
    zassert_true(done, "signaled worker did not finish");

    zassert_equal(OS_CondVarLock_Impl(&condvar_token), OS_SUCCESS);
    zassert_equal(wake_count, 1, "signal woke more than one worker");
    zassert_equal(wait_return_count, 1, "signal released extra waiters");
    permits = WORKER_COUNT - 1;
    zassert_equal(OS_CondVarBroadcast_Impl(&condvar_token), OS_SUCCESS);
    zassert_equal(OS_CondVarUnlock_Impl(&condvar_token), OS_SUCCESS);

    done = k_sem_take(&worker_done, K_MSEC(WATCHDOG_MS)) == 0;
    done = (k_sem_take(&worker_done, K_MSEC(WATCHDOG_MS)) == 0) && done;
    joined = k_thread_join(&worker_thread[0], K_MSEC(WATCHDOG_MS)) == 0;
    joined = (k_thread_join(&worker_thread[1], K_MSEC(WATCHDOG_MS)) == 0) && joined;
    joined = (k_thread_join(&worker_thread[2], K_MSEC(WATCHDOG_MS)) == 0) && joined;
    if (joined)
    {
        workers_started = false;
    }

    zassert_true(done, "broadcast worker did not finish");
    zassert_true(joined, "workers did not terminate");
    zassert_equal(wake_count, WORKER_COUNT);
    zassert_equal(wait_return_count, WORKER_COUNT);
    zassert_equal(worker_status[0], OS_SUCCESS);
    zassert_equal(worker_status[1], OS_SUCCESS);
    zassert_equal(worker_status[2], OS_SUCCESS);
    zassert_equal(OS_CondVarDelete_Impl(&condvar_token), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar, test_errors_and_deferred_timed_wait)
{
    OS_time_t deadline = OS_TimeAssembleFromMilliseconds(0, 1);

    zassert_equal(OS_CondVarCreate_Impl(&condvar_token, 0), OS_SUCCESS);
    zassert_not_equal(OS_CondVarUnlock_Impl(&condvar_token), OS_SUCCESS);
    zassert_equal(OS_CondVarLock_Impl(&condvar_token), OS_SUCCESS);
    zassert_equal(OS_CondVarTimedWait_Impl(&condvar_token, &deadline),
                  OS_ERR_NOT_IMPLEMENTED);
    zassert_equal(OS_CondVarUnlock_Impl(&condvar_token), OS_SUCCESS,
                  "unsupported timed wait did not preserve mutex ownership");
    zassert_equal(OS_CondVarDelete_Impl(&condvar_token), OS_SUCCESS);
}

ZTEST_SUITE(osal_zephyr_condvar, NULL, NULL, NULL, condvar_after, NULL);
