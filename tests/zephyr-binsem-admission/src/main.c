/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "osapi.h"
#include "os-impl-binsem.h"
#include "os-shared-idmap.h"

#define WATCHDOG_MS  5000
#define WAIT_MS      1000
#define REPEAT_COUNT 12

BUILD_ASSERT(OS_MAX_BIN_SEMAPHORES == 1,
             "This fixture requires physical binary-semaphore slot reuse");

enum pause_point
{
    PAUSE_NONE,
    AFTER_VALIDATION,
    BEFORE_MUTEX,
    BEFORE_WAIT,
    AFTER_WAKE_MUTEX
};

enum operation
{
    GIVE,
    TAKE,
    TIMED_WAIT,
    TIMED_POLL
};

static K_THREAD_STACK_DEFINE(worker_stack, 2048);
static struct k_thread worker;
static K_SEM_DEFINE(reached, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);
static K_SEM_DEFINE(done, 0, 1);
static atomic_t pause_at;
static atomic_t after_wake_mutex_armed;
static bool worker_started;
static bool gate_after_wake_mutex;
static osal_id_t worker_id;
static enum operation worker_operation;
static int32 worker_result;

int32 __real_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token);
int __real_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);
int __real_z_impl_k_condvar_wait(struct k_condvar *condvar,
                                 struct k_mutex *mutex, k_timeout_t timeout);

static void pause_worker(enum pause_point point)
{
    if (k_current_get() == &worker && atomic_cas(&pause_at, point, PAUSE_NONE))
    {
        k_sem_give(&reached);
        zassert_equal(k_sem_take(&resume, K_MSEC(WATCHDOG_MS)), 0,
                      "controller did not release worker at point %d", point);
    }
}

/* Gate only after shared validation; it never changes the requested lock mode. */
int32 __wrap_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token)
{
    int32 status = __real_OS_ObjectIdGetById(mode, type, id, token);

    if (status == OS_SUCCESS && type == OS_OBJECT_TYPE_OS_BINSEM &&
        mode == OS_LOCK_MODE_NONE)
    {
        pause_worker(AFTER_VALIDATION);
    }

    return status;
}

int __wrap_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
    if (mutex == &OS_impl_bin_sem_table[0].lock)
    {
        /* condvar.c reacquires this mutex after every wake.  Pausing before
         * that lock makes the delete/recreate-before-recheck window exact. */
        if (k_current_get() == &worker &&
            atomic_cas(&after_wake_mutex_armed, 1, 0))
        {
            pause_worker(AFTER_WAKE_MUTEX);
        }

        pause_worker(BEFORE_MUTEX);
    }

    return __real_z_impl_k_mutex_lock(mutex, timeout);
}

int __wrap_z_impl_k_condvar_wait(struct k_condvar *condvar,
                                 struct k_mutex *mutex, k_timeout_t timeout)
{
    if (condvar == &OS_impl_bin_sem_table[0].changed &&
        mutex == &OS_impl_bin_sem_table[0].lock && k_current_get() == &worker)
    {
        pause_worker(BEFORE_WAIT);
        if (gate_after_wake_mutex)
        {
            atomic_set(&pause_at, AFTER_WAKE_MUTEX);
            atomic_set(&after_wake_mutex_armed, 1);
        }
    }

    return __real_z_impl_k_condvar_wait(condvar, mutex, timeout);
}

static void worker_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    switch (worker_operation)
    {
        case GIVE:
            worker_result = OS_BinSemGive(worker_id);
            break;
        case TAKE:
            worker_result = OS_BinSemTake(worker_id);
            break;
        case TIMED_WAIT:
            worker_result = OS_BinSemTimedWait(worker_id, WAIT_MS);
            break;
        case TIMED_POLL:
            worker_result = OS_BinSemTimedWait(worker_id, 0);
            break;
    }

    k_sem_give(&done);
}

static void start_worker(osal_id_t id, enum operation operation,
                         enum pause_point point, bool gate_after_wake)
{
    k_sem_reset(&reached);
    k_sem_reset(&resume);
    k_sem_reset(&done);
    atomic_set(&pause_at, point);
    atomic_clear(&after_wake_mutex_armed);
    gate_after_wake_mutex = gate_after_wake;
    worker_id = id;
    worker_operation = operation;
    worker_result = OS_ERROR;

    k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
                    worker_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0,
                    K_FOREVER);
    worker_started = true;
    k_thread_start(&worker);

    if (point != PAUSE_NONE)
    {
        zassert_equal(k_sem_take(&reached, K_MSEC(WATCHDOG_MS)), 0,
                      "worker missed pause point %d", point);
    }
}

static void resume_worker(void)
{
    k_sem_give(&resume);
}

static void join_worker(void)
{
    zassert_equal(k_sem_take(&done, K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not complete");
    zassert_equal(k_thread_join(&worker, K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not terminate");
    worker_started = false;
    gate_after_wake_mutex = false;
}

static void stop_worker(void)
{
    zassert_true(worker_started, "no worker to stop");
    k_thread_abort(&worker);
    zassert_equal(k_thread_join(&worker, K_MSEC(WATCHDOG_MS)), 0,
                  "stopped worker did not terminate");
    worker_started = false;
    gate_after_wake_mutex = false;
    atomic_clear(&after_wake_mutex_armed);
}

static void assert_same_slot_new_generation(osal_id_t old_id, osal_id_t new_id)
{
    osal_index_t old_index;
    osal_index_t new_index;

    zassert_not_equal(OS_ObjectIdToInteger(old_id), OS_ObjectIdToInteger(new_id));
    zassert_equal(OS_ConvertToArrayIndex(old_id, &old_index), OS_SUCCESS);
    zassert_equal(OS_ConvertToArrayIndex(new_id, &new_index), OS_SUCCESS);
    zassert_equal(old_index, new_index, "physical slot was not reused");
}

static void assert_new_empty(osal_id_t id)
{
    zassert_equal(OS_BinSemTimedWait(id, 0), OS_SEM_TIMEOUT,
                  "replacement semaphore unexpectedly has a token");
}

/* Return with the controller holding the persistent native mutex.  Its
 * acquisition can only succeed once the worker entered the condvar's atomic
 * unlock-and-wait sequence. */
static void release_worker_into_kernel_wait(osal_id_t id, bool gate_after_wake)
{
    start_worker(id, TAKE, BEFORE_WAIT, gate_after_wake);
    resume_worker();
    zassert_equal(k_mutex_lock(&OS_impl_bin_sem_table[0].lock,
                               K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not release mutex into kernel wait");
}

static void stale_give_does_not_mutate_replacement(enum pause_point point)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_BinSemCreate(&old_id, "old", OS_SEM_EMPTY, 0), OS_SUCCESS);
    start_worker(old_id, GIVE, point, false);
    zassert_equal(OS_BinSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_BinSemCreate(&new_id, "new", OS_SEM_EMPTY, 0), OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);
    assert_new_empty(new_id);

    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    assert_new_empty(new_id);
    zassert_equal(OS_BinSemGive(old_id), OS_ERR_INVALID_ID);
    zassert_equal(OS_BinSemDelete(new_id), OS_SUCCESS);
}

static void wake_to_replacement_returns_invalid(void)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_BinSemCreate(&old_id, "wait-old", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    release_worker_into_kernel_wait(old_id, true);

    /* Delete and Create take this same mutex recursively.  Keeping the
     * controller's outer lock prevents the old waiter from rechecking its
     * identity until the replacement is completely initialized. */
    zassert_equal(OS_BinSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_BinSemCreate(&new_id, "wait-new", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);
    zassert_equal(OS_BinSemGive(new_id), OS_SUCCESS);
    zassert_equal(k_mutex_unlock(&OS_impl_bin_sem_table[0].lock), 0);

    zassert_equal(k_sem_take(&reached, K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not reach post-wake mutex gate");
    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    zassert_equal(OS_BinSemTimedWait(new_id, 0), OS_SUCCESS,
                  "old waiter consumed replacement token");
    zassert_equal(OS_BinSemDelete(new_id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_give_after_validation_is_rejected_after_reuse)
{
    stale_give_does_not_mutate_replacement(AFTER_VALIDATION);
}

ZTEST(osal_zephyr_binsem_admission, test_give_before_mutex_is_rejected_after_reuse)
{
    stale_give_does_not_mutate_replacement(BEFORE_MUTEX);
}

ZTEST(osal_zephyr_binsem_admission, test_stop_after_validation_leaves_no_admission)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_BinSemCreate(&old_id, "abort-old", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    start_worker(old_id, GIVE, AFTER_VALIDATION, false);
    stop_worker();
    zassert_equal(OS_BinSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_BinSemCreate(&new_id, "abort-new", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);
    assert_new_empty(new_id);
    zassert_equal(OS_BinSemDelete(new_id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_delete_fails_while_waiter_holds_mutex)
{
    osal_id_t id;
    osal_id_t found_id;

    zassert_equal(OS_BinSemCreate(&id, "locked-wait", OS_SEM_EMPTY, 0), OS_SUCCESS);
    start_worker(id, TAKE, BEFORE_WAIT, false);
    zassert_equal(OS_BinSemDelete(id), OS_SEM_FAILURE);
    zassert_equal(OS_BinSemGetIdByName(&found_id, "locked-wait"), OS_SUCCESS);
    zassert_equal(OS_ObjectIdToInteger(found_id), OS_ObjectIdToInteger(id));

    resume_worker();
    zassert_equal(k_mutex_lock(&OS_impl_bin_sem_table[0].lock,
                               K_MSEC(WATCHDOG_MS)), 0);
    zassert_equal(k_mutex_unlock(&OS_impl_bin_sem_table[0].lock), 0);
    zassert_equal(OS_BinSemFlush(id), OS_SUCCESS);
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(OS_BinSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_woken_waiter_rejects_reused_slot)
{
    wake_to_replacement_returns_invalid();
}

ZTEST(osal_zephyr_binsem_admission, test_stop_kernel_waiter_allows_delete_and_reuse)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_BinSemCreate(&old_id, "stop-wait-old", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    release_worker_into_kernel_wait(old_id, false);
    stop_worker();
    zassert_equal(OS_BinSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_BinSemCreate(&new_id, "stop-wait-new", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);
    zassert_equal(k_mutex_unlock(&OS_impl_bin_sem_table[0].lock), 0);
    assert_new_empty(new_id);
    zassert_equal(OS_BinSemDelete(new_id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_timed_wait_times_out_normally)
{
    osal_id_t id;

    zassert_equal(OS_BinSemCreate(&id, "timeout", OS_SEM_EMPTY, 0), OS_SUCCESS);
    zassert_equal(OS_BinSemTimedWait(id, 20), OS_SEM_TIMEOUT);
    zassert_equal(OS_BinSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission,
      test_timeout_wake_rejects_reused_slot_before_mutex_reacquisition)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_BinSemCreate(&old_id, "timeout-old", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    start_worker(old_id, TIMED_WAIT, BEFORE_WAIT, true);
    resume_worker();

    /* The worker's one-second wait has completed, but condvar.c has not yet
     * reacquired the persistent mutex. Delete and reuse the physical slot in
     * that interval so the identity check must override the stale timeout. */
    zassert_equal(k_sem_take(&reached, K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not reach timeout post-wake mutex gate");
    zassert_equal(OS_BinSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_BinSemCreate(&new_id, "timeout-new", OS_SEM_FULL, 0),
                  OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);

    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    zassert_equal(OS_BinSemTimedWait(new_id, 0), OS_SUCCESS,
                  "timed-out old waiter consumed replacement token");
    zassert_equal(OS_BinSemDelete(new_id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_flush_releases_current_waiter_without_token)
{
    osal_id_t id;

    zassert_equal(OS_BinSemCreate(&id, "flush-wait", OS_SEM_EMPTY, 0), OS_SUCCESS);
    release_worker_into_kernel_wait(id, false);
    zassert_equal(k_mutex_unlock(&OS_impl_bin_sem_table[0].lock), 0);
    zassert_equal(OS_BinSemFlush(id), OS_SUCCESS);
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    assert_new_empty(id);
    zassert_equal(OS_BinSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_flush_preserves_available_token)
{
    osal_id_t id;

    zassert_equal(OS_BinSemCreate(&id, "flush-full", OS_SEM_FULL, 0), OS_SUCCESS);
    zassert_equal(OS_BinSemFlush(id), OS_SUCCESS);
    zassert_equal(OS_BinSemTimedWait(id, 0), OS_SUCCESS);
    zassert_equal(OS_BinSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_give_releases_current_waiter)
{
    osal_id_t id;

    zassert_equal(OS_BinSemCreate(&id, "give-wait", OS_SEM_EMPTY, 0), OS_SUCCESS);
    release_worker_into_kernel_wait(id, false);
    zassert_equal(k_mutex_unlock(&OS_impl_bin_sem_table[0].lock), 0);
    zassert_equal(OS_BinSemGive(id), OS_SUCCESS);
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    assert_new_empty(id);
    zassert_equal(OS_BinSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_zero_timeout_does_not_wait_for_mutex)
{
    osal_id_t id;

    zassert_equal(OS_BinSemCreate(&id, "contended", OS_SEM_EMPTY, 0), OS_SUCCESS);
    zassert_equal(k_mutex_lock(&OS_impl_bin_sem_table[0].lock, K_FOREVER), 0);
    start_worker(id, TIMED_POLL, PAUSE_NONE, false);
    join_worker();
    zassert_equal(worker_result, OS_SEM_TIMEOUT);
    zassert_equal(k_mutex_unlock(&OS_impl_bin_sem_table[0].lock), 0);
    zassert_equal(OS_BinSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_binsem_admission, test_repeated_wake_reuse_interleaving)
{
    for (unsigned int i = 0; i < REPEAT_COUNT; ++i)
    {
        wake_to_replacement_returns_invalid();
    }
}

ZTEST(osal_zephyr_binsem_admission, test_quiescent_shutdown_reinitialization)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_BinSemCreate(&old_id, "teardown-old", OS_SEM_EMPTY, 0),
                  OS_SUCCESS);
    zassert_equal(OS_BinSemDelete(old_id), OS_SUCCESS);
    OS_ApplicationShutdown(true);
    zassert_equal(OS_API_Init(), OS_SUCCESS);

    zassert_equal(OS_BinSemCreate(&new_id, "teardown-new", OS_SEM_FULL, 0),
                  OS_SUCCESS);
    zassert_equal(OS_BinSemTimedWait(new_id, 0), OS_SUCCESS);
    zassert_equal(OS_BinSemDelete(new_id), OS_SUCCESS);
}

static void *suite_setup(void)
{
    zassert_equal(OS_API_Init(), OS_SUCCESS);
    return NULL;
}

ZTEST_SUITE(osal_zephyr_binsem_admission, NULL, suite_setup, NULL, NULL, NULL);
