/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "osapi.h"
#include "os-impl-mutex.h"
#include "os-shared-idmap.h"
#include "os-shared-mutex.h"

#define WATCHDOG_MS  5000
#define REPEAT_COUNT 12

BUILD_ASSERT(OS_MAX_MUTEXES == 1,
             "This fixture requires physical mutex slot reuse");
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "This fixture requires UP scheduling");
BUILD_ASSERT(CONFIG_ZTEST_THREAD_PRIORITY > 1,
             "The worker must preempt the controller until it blocks");

enum pause_point
{
    PAUSE_NONE,
    AFTER_VALIDATION,
    BEFORE_NATIVE_LOCK,
    AFTER_NATIVE_LOCK,
    AFTER_TAKE,
    CALLBACK_AFTER_GIVE
};

enum operation
{
    MUTEX_TAKE,
    MUTEX_GIVE,
    MUTEX_DELETE,
    MUTEX_TAKE_AND_GIVE,
    MUTEX_TAKE_HOLD_AND_GIVE,
    MUTEX_CALLBACK_GIVE
};

static K_THREAD_STACK_DEFINE(worker_stack, 2048);
static struct k_thread worker;
static K_SEM_DEFINE(reached, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);
static K_SEM_DEFINE(done, 0, 1);
static atomic_t pause_at;
static atomic_t after_native_lock_armed;
static bool worker_started;
static bool callback_enabled;
static bool shared_gap_probe_enabled;
static bool shared_gap_cleared_before_provider;
static osal_id_t callback_new_id;
static int32 callback_status;
static osal_id_t worker_id;
static enum operation worker_operation;
static int32 worker_result;
static int32 worker_release_result;

int32 __real_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token);
int __real_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);

static void pause_worker(enum pause_point point)
{
    if (k_current_get() == &worker && atomic_cas(&pause_at, point, PAUSE_NONE))
    {
        k_sem_give(&reached);
        zassert_equal(k_sem_take(&resume, K_MSEC(WATCHDOG_MS)), 0,
                      "controller did not release worker at point %d", point);
    }
}

/* This wrapper observes validation but never changes the shared lock mode. */
int32 __wrap_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token)
{
    int32 status = __real_OS_ObjectIdGetById(mode, type, id, token);

    if (status == OS_SUCCESS && type == OS_OBJECT_TYPE_OS_MUTEX &&
        mode == OS_LOCK_MODE_NONE)
    {
        if (callback_enabled && k_current_get() == &worker)
        {
            callback_enabled = false;
            callback_status = OS_MutSemDelete(id);
            if (callback_status == OS_SUCCESS)
            {
                callback_status = OS_MutSemCreate(&callback_new_id, "callback-new", 0);
            }
            if (callback_status == OS_SUCCESS)
            {
                callback_status = OS_MutSemTake(callback_new_id);
            }
        }

        pause_worker(AFTER_VALIDATION);
    }

    return status;
}

int __wrap_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
    int status;

    if (mutex != &OS_impl_mutex_table[0].lock)
    {
        return __real_z_impl_k_mutex_lock(mutex, timeout);
    }

    pause_worker(BEFORE_NATIVE_LOCK);
    status = __real_z_impl_k_mutex_lock(mutex, timeout);

    if (status == 0 && shared_gap_probe_enabled && k_current_get() == &worker)
    {
        /* OS_MutSemGive clears this shared field before calling Give_Impl,
         * whose ID comparison occurs after this wrapped native acquisition. */
        shared_gap_cleared_before_provider =
            OS_ObjectIdEqual(OS_mutex_table[0].last_owner, OS_OBJECT_ID_UNDEFINED);
    }

    if (status == 0 && k_current_get() == &worker &&
        atomic_cas(&after_native_lock_armed, 1, 0))
    {
        atomic_set(&pause_at, AFTER_NATIVE_LOCK);
        pause_worker(AFTER_NATIVE_LOCK);
    }

    return status;
}

static void worker_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    worker_release_result = OS_ERROR;
    switch (worker_operation)
    {
        case MUTEX_TAKE:
            worker_result = OS_MutSemTake(worker_id);
            break;
        case MUTEX_GIVE:
            worker_result = OS_MutSemGive(worker_id);
            break;
        case MUTEX_DELETE:
            worker_result = OS_MutSemDelete(worker_id);
            break;
        case MUTEX_TAKE_AND_GIVE:
            worker_result = OS_MutSemTake(worker_id);
            if (worker_result == OS_SUCCESS)
            {
                worker_release_result = OS_MutSemGive(worker_id);
            }
            break;
        case MUTEX_TAKE_HOLD_AND_GIVE:
            worker_result = OS_MutSemTake(worker_id);
            if (worker_result == OS_SUCCESS)
            {
                atomic_set(&pause_at, AFTER_TAKE);
                pause_worker(AFTER_TAKE);
                worker_release_result = OS_MutSemGive(worker_id);
            }
            break;
        case MUTEX_CALLBACK_GIVE:
            worker_result = OS_MutSemGive(worker_id);
            pause_worker(CALLBACK_AFTER_GIVE);
            if (callback_status == OS_SUCCESS)
            {
                worker_release_result = OS_MutSemGive(callback_new_id);
            }
            break;
    }

    k_sem_give(&done);
}

static void start_worker(osal_id_t id, enum operation operation,
                         enum pause_point point, bool gate_after_native_lock)
{
    k_sem_reset(&reached);
    k_sem_reset(&resume);
    k_sem_reset(&done);
    atomic_set(&pause_at, point);
    atomic_set(&after_native_lock_armed, gate_after_native_lock ? 1 : 0);
    worker_id = id;
    worker_operation = operation;
    worker_result = OS_ERROR;
    worker_release_result = OS_ERROR;

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
}

static void stop_worker_after_validation(void)
{
    zassert_true(worker_started, "no worker to stop");
    k_thread_abort(&worker);
    zassert_equal(k_thread_join(&worker, K_MSEC(WATCHDOG_MS)), 0,
                  "stopped worker did not terminate");
    worker_started = false;
    atomic_clear(&after_native_lock_armed);
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

static void stale_take_reuse(enum pause_point point)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_MutSemCreate(&old_id, "take-old", 0), OS_SUCCESS);
    start_worker(old_id, MUTEX_TAKE, point, false);
    zassert_equal(OS_MutSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_MutSemCreate(&new_id, "take-new", 0), OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);

    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    zassert_equal(OS_MutSemDelete(new_id), OS_SUCCESS,
                  "stale Take changed replacement ownership");
}

static void stale_give_reuse(bool seed_shared_owner)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_MutSemCreate(&old_id, "give-old", 0), OS_SUCCESS);
    shared_gap_probe_enabled = false;
    shared_gap_cleared_before_provider = false;
    start_worker(old_id, MUTEX_GIVE, AFTER_VALIDATION, false);
    zassert_equal(OS_MutSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_MutSemCreate(&new_id, "give-new", 0), OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);
    if (seed_shared_owner)
    {
        OS_mutex_table[0].last_owner = OS_ObjectIdFromInteger(0x5a5a5a5aU);
        shared_gap_probe_enabled = true;
    }

    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    if (seed_shared_owner)
    {
        zassert_true(OS_ObjectIdEqual(OS_mutex_table[0].last_owner,
                                      OS_OBJECT_ID_UNDEFINED),
                     "shared last_owner was not cleared before stale Give rejection");
        zassert_true(shared_gap_cleared_before_provider,
                     "shared last_owner was not clear at native provider entry");
    }
    shared_gap_probe_enabled = false;
    zassert_equal(OS_MutSemDelete(new_id), OS_SUCCESS,
                  "stale Give changed replacement ownership");
}

ZTEST(osal_zephyr_mutex_lifecycle, test_stale_take_after_validation_rejects_reuse)
{
    stale_take_reuse(AFTER_VALIDATION);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_stale_take_before_native_lock_rejects_reuse)
{
    stale_take_reuse(BEFORE_NATIVE_LOCK);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_stale_give_rejects_reuse)
{
    stale_give_reuse(false);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_same_thread_stale_give_keeps_replacement_depth)
{
    osal_id_t old_id;

    callback_enabled = true;
    callback_status = OS_ERROR;
    callback_new_id = OS_OBJECT_ID_UNDEFINED;
    zassert_equal(OS_MutSemCreate(&old_id, "callback-old", 0), OS_SUCCESS);
    start_worker(old_id, MUTEX_CALLBACK_GIVE, CALLBACK_AFTER_GIVE, false);
    zassert_equal(callback_status, OS_SUCCESS);
    assert_same_slot_new_generation(old_id, callback_new_id);
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    zassert_equal(OS_MutSemDelete(callback_new_id), OS_SEM_FAILURE,
                  "stale Give reduced replacement depth");

    resume_worker();
    join_worker();
    zassert_equal(worker_release_result, OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(callback_new_id), OS_SUCCESS);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_delete_requires_final_recursive_give)
{
    osal_id_t id;

    zassert_equal(OS_MutSemCreate(&id, "recursive", 0), OS_SUCCESS);
    zassert_equal(OS_MutSemTake(id), OS_SUCCESS);
    zassert_equal(OS_MutSemTake(id), OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(id), OS_SEM_FAILURE);

    start_worker(id, MUTEX_DELETE, PAUSE_NONE, false);
    join_worker();
    zassert_equal(worker_result, OS_SEM_FAILURE);

    zassert_equal(OS_MutSemGive(id), OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(id), OS_SEM_FAILURE);
    start_worker(id, MUTEX_DELETE, PAUSE_NONE, false);
    join_worker();
    zassert_equal(worker_result, OS_SEM_FAILURE);

    zassert_equal(OS_MutSemGive(id), OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_queued_take_handoff_rejects_delete)
{
    osal_id_t id;

    zassert_equal(OS_MutSemCreate(&id, "handoff", 0), OS_SUCCESS);
    zassert_equal(OS_MutSemTake(id), OS_SUCCESS);
    start_worker(id, MUTEX_TAKE_HOLD_AND_GIVE, BEFORE_NATIVE_LOCK, true);
    zassert_equal(OS_MutSemDelete(id), OS_SEM_FAILURE,
                  "Delete succeeded while queued Take held pre-lock gate");

    /* Worker priority 1 preempts controller priority 2 on this UP fixture.
     * It runs from the gate into the native wait before resume returns. */
    resume_worker();
    zassert_equal(OS_MutSemGive(id), OS_SUCCESS);
    zassert_equal(k_sem_take(&reached, K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not reach post-handoff native lock gate");
    zassert_equal(OS_MutSemDelete(id), OS_SEM_FAILURE,
                  "Delete succeeded before handed-off Take validated");

    resume_worker();
    zassert_equal(k_sem_take(&reached, K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not report successful public Take");
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(id), OS_SEM_FAILURE,
                  "Delete succeeded while handed-off Take owned the mutex");

    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(worker_release_result, OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_wrong_owner_give_fails)
{
    osal_id_t id;

    zassert_equal(OS_MutSemCreate(&id, "wrong-owner", 0), OS_SUCCESS);
    zassert_equal(OS_MutSemTake(id), OS_SUCCESS);
    start_worker(id, MUTEX_GIVE, PAUSE_NONE, false);
    join_worker();
    zassert_equal(worker_result, OS_SEM_FAILURE);
    zassert_equal(OS_MutSemGive(id), OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_normal_contention_transfers_owner)
{
    osal_id_t id;

    zassert_equal(OS_MutSemCreate(&id, "contention", 0), OS_SUCCESS);
    zassert_equal(OS_MutSemTake(id), OS_SUCCESS);
    start_worker(id, MUTEX_TAKE_AND_GIVE, PAUSE_NONE, false);
    zassert_equal(OS_MutSemGive(id), OS_SUCCESS);
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(worker_release_result, OS_SUCCESS);
    zassert_equal(OS_MutSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_abort_after_validation_leaves_no_admission)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_MutSemCreate(&old_id, "abort-old", 0), OS_SUCCESS);
    start_worker(old_id, MUTEX_TAKE, AFTER_VALIDATION, false);
    stop_worker_after_validation();
    zassert_equal(OS_MutSemDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_MutSemCreate(&new_id, "abort-new", 0), OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);
    zassert_equal(OS_MutSemDelete(new_id), OS_SUCCESS);
}

ZTEST(osal_zephyr_mutex_lifecycle,
      test_known_shared_last_owner_clear_before_stale_give_reject)
{
    stale_give_reuse(true);
}

ZTEST(osal_zephyr_mutex_lifecycle, test_repeated_stale_take_reuse)
{
    for (unsigned int i = 0; i < REPEAT_COUNT; ++i)
    {
        stale_take_reuse(BEFORE_NATIVE_LOCK);
    }
}

static void *suite_setup(void)
{
    zassert_equal(OS_API_Init(), OS_SUCCESS);
    return NULL;
}

ZTEST_SUITE(osal_zephyr_mutex_lifecycle, NULL, suite_setup, NULL, NULL, NULL);
