/*
 * Copyright (c) 2026 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "harness.h"
#include "os-shared-condvar.h"
#include "os-shared-idmap.h"

#define WORKER_COUNT      3
#define WORKER_STACK_SIZE 2048
#define WATCHDOG_TIMEOUT  K_SECONDS(2)

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

int main(void)
{
    struct harness     h;
    OS_condvar_prop_t prop;
    int32              status;
    bool               ready;
    bool               done;
    bool               joined;

    harness_start(&h, "condvar");

    status = OS_CondVarCreate_Impl(&condvar_token, 0);
    harness_check(&h, status == OS_SUCCESS, "create");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }

    status = OS_CondVarUnlock_Impl(&condvar_token);
    harness_check(&h, status != OS_SUCCESS, "unlock-without-lock-fails");

    status = OS_CondVarLock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "lock");
    status = OS_CondVarUnlock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "unlock");

    status = OS_CondVarSignal_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "signal-without-waiter");
    status = OS_CondVarBroadcast_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "broadcast-without-waiter");

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
                    NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_create(&worker_thread[1], worker_stack_1,
                    K_THREAD_STACK_SIZEOF(worker_stack_1), worker_1,
                    NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_create(&worker_thread[2], worker_stack_2,
                    K_THREAD_STACK_SIZEOF(worker_stack_2), worker_2,
                    NULL, NULL, NULL, 5, 0, K_NO_WAIT);

    ready = k_sem_take(&worker_ready, WATCHDOG_TIMEOUT) == 0;
    ready = (k_sem_take(&worker_ready, WATCHDOG_TIMEOUT) == 0) && ready;
    ready = (k_sem_take(&worker_ready, WATCHDOG_TIMEOUT) == 0) && ready;
    harness_check(&h, ready, "all-waiters-ready");

    status = OS_CondVarLock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "lock-before-signal");
    done = k_sem_take(&worker_done, K_NO_WAIT) == 0;
    harness_check(&h, !done && wait_return_count == 0,
                  "notification-is-not-sticky");
    permits = 1;
    status  = OS_CondVarSignal_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "signal-one");
    done = k_sem_take(&worker_done, K_MSEC(20)) == 0;
    harness_check(&h, !done, "waiter-reacquires-before-return");
    status = OS_CondVarUnlock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "unlock-after-signal");

    done = k_sem_take(&worker_done, WATCHDOG_TIMEOUT) == 0;
    harness_check(&h, done, "one-waiter-finished");

    status = OS_CondVarLock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "lock-after-signal");
    harness_check(&h,
                  wake_count == 1 && wait_return_count == 1 && permits == 0,
                  "signal-wakes-one");
    permits = WORKER_COUNT - 1;
    status  = OS_CondVarBroadcast_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "broadcast-remaining");
    status = OS_CondVarUnlock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "unlock-after-broadcast");

    done = k_sem_take(&worker_done, WATCHDOG_TIMEOUT) == 0;
    done = (k_sem_take(&worker_done, WATCHDOG_TIMEOUT) == 0) && done;
    harness_check(&h, done, "broadcast-waiters-finished");

    joined = k_thread_join(&worker_thread[0], WATCHDOG_TIMEOUT) == 0;
    joined = (k_thread_join(&worker_thread[1], WATCHDOG_TIMEOUT) == 0) && joined;
    joined = (k_thread_join(&worker_thread[2], WATCHDOG_TIMEOUT) == 0) && joined;
    harness_check(&h, joined, "workers-joined");
    harness_check(&h,
                  wake_count == WORKER_COUNT &&
                      wait_return_count == WORKER_COUNT && permits == 0,
                  "all-waiters-finished");
    harness_check(&h,
                  worker_status[0] == OS_SUCCESS &&
                      worker_status[1] == OS_SUCCESS &&
                      worker_status[2] == OS_SUCCESS,
                  "worker-statuses");

    status = OS_CondVarLock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "lock-before-timedwait");
    status = OS_CondVarTimedWait_Impl(&condvar_token, NULL);
    harness_check(&h, status == OS_ERR_NOT_IMPLEMENTED,
                  "timedwait-explicitly-unsupported");
    status = OS_CondVarUnlock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "timedwait-preserves-lock");

    status = OS_CondVarGetInfo_Impl(&condvar_token, &prop);
    harness_check(&h, status == OS_SUCCESS, "getinfo");

    status = OS_CondVarDelete_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "delete");
    status = OS_CondVarCreate_Impl(&condvar_token, 0);
    harness_check(&h, status == OS_SUCCESS, "recreate");
    status = OS_CondVarLock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "lock-recreated");
    status = OS_CondVarUnlock_Impl(&condvar_token);
    harness_check(&h, status == OS_SUCCESS, "unlock-recreated");

    return harness_finish(&h);
}
