/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "osapi.h"
#include "os-impl-condvar.h"
#include "os-shared-idmap.h"

#define WATCHDOG_MS  5000
#define REPEAT_COUNT 12

BUILD_ASSERT(OS_MAX_CONDVARS == 1,
             "This fixture requires physical condition-variable slot reuse");
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "This fixture requires UP scheduling");
BUILD_ASSERT(CONFIG_ZTEST_THREAD_PRIORITY > 1,
             "The worker must preempt the controller until it blocks");

enum pause_point
{
    PAUSE_NONE,
    AFTER_VALIDATION,
    BEFORE_APPLICATION_LOCK,
    AFTER_APPLICATION_LOCK,
    BEFORE_STATE_LOCK,
    BEFORE_KERNEL_WAIT,
    BEFORE_WAKE_APPLICATION_LOCK,
    AFTER_WAKE_APPLICATION_LOCK,
    AFTER_WAIT_RETURN,
    CALLBACK_AFTER_UNLOCK,
    IN_NATIVE_SIGNAL,
    IN_NATIVE_BROADCAST
};

enum operation
{
    WORKER_LOCK,
    WORKER_UNLOCK_CALLBACK,
    WORKER_DELETE,
    WORKER_WAIT,
    WORKER_WAIT_WITHOUT_LOCK,
    WORKER_SIGNAL,
    WORKER_BROADCAST
};

static K_THREAD_STACK_DEFINE(worker_stack, 2048);
static struct k_thread worker;
static K_SEM_DEFINE(reached, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);
static K_SEM_DEFINE(done, 0, 1);
static atomic_t pause_at;
static atomic_t after_wake_application_lock_armed;
static bool worker_started;
static bool worker_wait_phase;
static bool callback_enabled;
static bool callback_ok;
static int native_signal_calls;
static int native_broadcast_calls;
static osal_id_t callback_old_id;
static osal_id_t callback_new_id;
static osal_id_t worker_id;
static enum operation worker_operation;
static int32 worker_lock_result;
static int32 worker_result;
static int32 worker_release_result;

int32 __real_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token);
int __real_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);
int __real_z_impl_k_condvar_wait(struct k_condvar *condvar,
                                 struct k_mutex *mutex, k_timeout_t timeout);
int __real_z_impl_k_condvar_signal(struct k_condvar *condvar);
int __real_z_impl_k_condvar_broadcast(struct k_condvar *condvar);

static void pause_worker(enum pause_point point)
{
    if (k_current_get() == &worker && atomic_cas(&pause_at, point, PAUSE_NONE))
    {
        k_sem_give(&reached);
        zassert_equal(k_sem_take(&resume, K_MSEC(WATCHDOG_MS)), 0,
                      "controller did not release worker at point %d", point);
    }
}

/* This observes successful shared NONE validation without changing its mode. */
int32 __wrap_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token)
{
    int32 status = __real_OS_ObjectIdGetById(mode, type, id, token);

    if (status == OS_SUCCESS && type == OS_OBJECT_TYPE_OS_CONDVAR &&
        mode == OS_LOCK_MODE_NONE && k_current_get() == &worker)
    {
        if (callback_enabled && worker_operation == WORKER_UNLOCK_CALLBACK)
        {
            callback_enabled = false;
            callback_ok = OS_CondVarDelete(callback_old_id) == OS_SUCCESS;
            if (callback_ok)
            {
                callback_ok = OS_CondVarCreate(&callback_new_id, "callback-new", 0) == OS_SUCCESS;
            }
            if (callback_ok)
            {
                callback_ok = OS_CondVarLock(callback_new_id) == OS_SUCCESS;
            }
        }

        if (worker_operation != WORKER_WAIT || worker_wait_phase)
        {
            pause_worker(AFTER_VALIDATION);
        }
    }

    return status;
}

int __wrap_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
    int status;

    if (k_current_get() == &worker && mutex == &OS_impl_condvar_table[0].lock)
    {
        if (worker_operation == WORKER_LOCK)
        {
            pause_worker(BEFORE_APPLICATION_LOCK);
            status = __real_z_impl_k_mutex_lock(mutex, timeout);
            if (status == 0)
            {
                /* The provider has application ownership but has not yet
                 * taken state_lock or incremented its public depth. */
                pause_worker(AFTER_APPLICATION_LOCK);
            }
            return status;
        }

        /* k_condvar_wait() reacquires this mutex before it returns. */
        if (atomic_cas(&after_wake_application_lock_armed, 1, 0))
        {
            pause_worker(BEFORE_WAKE_APPLICATION_LOCK);
            status = __real_z_impl_k_mutex_lock(mutex, timeout);
            if (status == 0)
            {
                pause_worker(AFTER_WAKE_APPLICATION_LOCK);
            }
            return status;
        }
    }

    if (k_current_get() == &worker && mutex == &OS_impl_condvar_table[0].state_lock &&
        (worker_operation == WORKER_SIGNAL || worker_operation == WORKER_BROADCAST))
    {
        pause_worker(BEFORE_STATE_LOCK);
    }

    return __real_z_impl_k_mutex_lock(mutex, timeout);
}

int __wrap_z_impl_k_condvar_wait(struct k_condvar *condvar,
                                 struct k_mutex *mutex, k_timeout_t timeout)
{
    if (k_current_get() == &worker && worker_operation == WORKER_WAIT &&
        condvar == &OS_impl_condvar_table[0].changed &&
        mutex == &OS_impl_condvar_table[0].lock)
    {
        pause_worker(BEFORE_KERNEL_WAIT);
        /* Arm before the real atomic unlock-and-wait.  The next application
         * lock is the native wake reacquisition inside k_condvar_wait(). */
        atomic_set(&after_wake_application_lock_armed, 1);
    }

    return __real_z_impl_k_condvar_wait(condvar, mutex, timeout);
}

int __wrap_z_impl_k_condvar_signal(struct k_condvar *condvar)
{
    if (condvar == &OS_impl_condvar_table[0].changed)
    {
        ++native_signal_calls;
        if (k_current_get() == &worker && worker_operation == WORKER_SIGNAL)
        {
            pause_worker(IN_NATIVE_SIGNAL);
        }
    }
    return __real_z_impl_k_condvar_signal(condvar);
}

int __wrap_z_impl_k_condvar_broadcast(struct k_condvar *condvar)
{
    if (condvar == &OS_impl_condvar_table[0].changed)
    {
        ++native_broadcast_calls;
        if (k_current_get() == &worker && worker_operation == WORKER_BROADCAST)
        {
            pause_worker(IN_NATIVE_BROADCAST);
        }
    }
    return __real_z_impl_k_condvar_broadcast(condvar);
}

static void worker_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    worker_lock_result    = OS_ERROR;
    worker_release_result = OS_ERROR;
    switch (worker_operation)
    {
        case WORKER_LOCK:
            worker_result = OS_CondVarLock(worker_id);
            if (worker_result == OS_SUCCESS)
            {
                worker_release_result = OS_CondVarUnlock(worker_id);
            }
            break;

        case WORKER_UNLOCK_CALLBACK:
            worker_result = OS_CondVarUnlock(worker_id);
            pause_worker(CALLBACK_AFTER_UNLOCK);
            if (callback_ok)
            {
                worker_release_result = OS_CondVarUnlock(callback_new_id);
            }
            break;

        case WORKER_DELETE:
            worker_result = OS_CondVarDelete(worker_id);
            break;

        case WORKER_WAIT:
            worker_lock_result = OS_CondVarLock(worker_id);
            if (worker_lock_result == OS_SUCCESS)
            {
                worker_wait_phase = true;
                worker_result = OS_CondVarWait(worker_id);
                worker_wait_phase = false;
                if (worker_result == OS_SUCCESS)
                {
                    pause_worker(AFTER_WAIT_RETURN);
                    worker_release_result = OS_CondVarUnlock(worker_id);
                }
            }
            else
            {
                worker_result = worker_lock_result;
            }
            break;

        case WORKER_WAIT_WITHOUT_LOCK:
            worker_result = OS_CondVarWait(worker_id);
            break;

        case WORKER_SIGNAL:
            worker_result = OS_CondVarSignal(worker_id);
            break;

        case WORKER_BROADCAST:
            worker_result = OS_CondVarBroadcast(worker_id);
            break;
    }

    k_sem_give(&done);
}

static void wait_for_pause(enum pause_point point)
{
    zassert_equal(k_sem_take(&reached, K_MSEC(WATCHDOG_MS)), 0,
                  "worker missed pause point %d", point);
}

static void start_worker(osal_id_t id, enum operation operation, enum pause_point point)
{
    k_sem_reset(&reached);
    k_sem_reset(&resume);
    k_sem_reset(&done);
    atomic_set(&pause_at, point);
    atomic_clear(&after_wake_application_lock_armed);
    worker_wait_phase = false;
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
        wait_for_pause(point);
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
    atomic_clear(&after_wake_application_lock_armed);
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

static void stale_lock_reuse(enum pause_point point)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_CondVarCreate(&old_id, "lock-old", 0), OS_SUCCESS);
    start_worker(old_id, WORKER_LOCK, point);
    zassert_equal(OS_CondVarDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_CondVarCreate(&new_id, "lock-new", 0), OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);

    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    zassert_equal(OS_CondVarDelete(new_id), OS_SUCCESS,
                  "stale Lock retained replacement ownership");
}

static void stale_notify_reuse(enum operation operation, enum pause_point point)
{
    osal_id_t old_id;
    osal_id_t new_id;

    native_signal_calls = 0;
    native_broadcast_calls = 0;
    zassert_equal(OS_CondVarCreate(&old_id, "notify-old", 0), OS_SUCCESS);
    start_worker(old_id, operation, point);
    zassert_equal(OS_CondVarDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_CondVarCreate(&new_id, "notify-new", 0), OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);

    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    zassert_equal(native_signal_calls, 0, "stale Signal notified replacement");
    zassert_equal(native_broadcast_calls, 0, "stale Broadcast notified replacement");
    zassert_equal(OS_CondVarDelete(new_id), OS_SUCCESS);
}

/* A successful raw lock proves the worker crossed Zephyr's atomic
 * unlock-and-wait boundary and is sleeping with no application ownership. */
static void wait_until_worker_sleeps(void)
{
    zassert_equal(k_mutex_lock(&OS_impl_condvar_table[0].lock, K_MSEC(WATCHDOG_MS)), 0,
                  "worker did not enter kernel wait");
    zassert_equal(k_mutex_unlock(&OS_impl_condvar_table[0].lock), 0);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_stale_lock_after_validation_rejects_reuse)
{
    stale_lock_reuse(AFTER_VALIDATION);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_stale_lock_before_application_lock_rejects_reuse)
{
    stale_lock_reuse(BEFORE_APPLICATION_LOCK);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_repeated_stale_lock_reuse)
{
    for (unsigned int i = 0; i < REPEAT_COUNT; ++i)
    {
        stale_lock_reuse(BEFORE_APPLICATION_LOCK);
    }
}

ZTEST(osal_zephyr_condvar_lifecycle,
      test_delete_rejects_queued_lock_after_native_acquisition_before_depth)
{
    osal_id_t id;

    zassert_equal(OS_CondVarCreate(&id, "lock-handoff", 0), OS_SUCCESS);
    zassert_equal(OS_CondVarLock(id), OS_SUCCESS);
    start_worker(id, WORKER_LOCK, BEFORE_APPLICATION_LOCK);
    atomic_set(&pause_at, AFTER_APPLICATION_LOCK);
    resume_worker();
    /* The higher-priority worker queues on the native mutex, then reaches
     * AFTER_APPLICATION_LOCK as soon as this final controller Unlock hands it
     * ownership. */
    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    wait_for_pause(AFTER_APPLICATION_LOCK);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete succeeded after queued native Lock but before depth update");
    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(worker_release_result, OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_stale_signal_after_validation_does_not_notify_reuse)
{
    stale_notify_reuse(WORKER_SIGNAL, AFTER_VALIDATION);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_stale_broadcast_before_state_lock_does_not_notify_reuse)
{
    stale_notify_reuse(WORKER_BROADCAST, BEFORE_STATE_LOCK);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_same_thread_stale_unlock_keeps_replacement_depth)
{
    osal_id_t old_id;

    callback_enabled = true;
    callback_ok = false;
    callback_old_id = OS_OBJECT_ID_UNDEFINED;
    callback_new_id = OS_OBJECT_ID_UNDEFINED;
    zassert_equal(OS_CondVarCreate(&old_id, "callback-old", 0), OS_SUCCESS);
    callback_old_id = old_id;
    start_worker(old_id, WORKER_UNLOCK_CALLBACK, CALLBACK_AFTER_UNLOCK);
    zassert_true(callback_ok, "callback did not replace and lock the slot");
    assert_same_slot_new_generation(old_id, callback_new_id);
    zassert_equal(worker_result, OS_ERR_INVALID_ID);
    zassert_equal(OS_CondVarDelete(callback_new_id), OS_ERROR,
                  "stale Unlock reduced replacement depth");

    resume_worker();
    join_worker();
    zassert_equal(worker_release_result, OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(callback_new_id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_delete_rejects_self_and_other_recursive_owners)
{
    osal_id_t id;

    zassert_equal(OS_CondVarCreate(&id, "recursive", 0), OS_SUCCESS);
    zassert_equal(OS_CondVarLock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR);
    zassert_equal(OS_CondVarLock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR);

    start_worker(id, WORKER_DELETE, PAUSE_NONE);
    join_worker();
    zassert_equal(worker_result, OS_ERROR);

    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR);
    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_wait_without_owner_and_wrong_owner_fail)
{
    osal_id_t id;

    zassert_equal(OS_CondVarCreate(&id, "wait-owner", 0), OS_SUCCESS);
    zassert_equal(OS_CondVarWait(id), OS_ERROR);
    zassert_equal(OS_CondVarLock(id), OS_SUCCESS);
    start_worker(id, WORKER_WAIT_WITHOUT_LOCK, PAUSE_NONE);
    join_worker();
    zassert_equal(worker_result, OS_ERROR);
    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_recursive_wait_fails_and_preserves_depth)
{
    osal_id_t id;

    zassert_equal(OS_CondVarCreate(&id, "wait-recursive", 0), OS_SUCCESS);
    zassert_equal(OS_CondVarLock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarLock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarWait(id), OS_ERROR);
    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_wait_reservation_covers_sleep_wake_and_return)
{
    osal_id_t id;

    zassert_equal(OS_CondVarCreate(&id, "wait-lifetime", 0), OS_SUCCESS);
    start_worker(id, WORKER_WAIT, AFTER_VALIDATION);
    zassert_equal(worker_lock_result, OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete succeeded between shared validation and kernel wait");

    atomic_set(&pause_at, BEFORE_KERNEL_WAIT);
    resume_worker();
    wait_for_pause(BEFORE_KERNEL_WAIT);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete succeeded after Wait reserved the slot before native wait");
    resume_worker();
    wait_until_worker_sleeps();

    zassert_equal(OS_CondVarLock(id), OS_SUCCESS,
                  "sleeping waiter did not release application ownership");
    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete ignored a sleeping waiter reservation");

    atomic_set(&pause_at, BEFORE_WAKE_APPLICATION_LOCK);
    zassert_equal(OS_CondVarSignal(id), OS_SUCCESS);
    wait_for_pause(BEFORE_WAKE_APPLICATION_LOCK);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete ignored waiter reservation before native reacquisition");

    atomic_set(&pause_at, AFTER_WAKE_APPLICATION_LOCK);
    resume_worker();
    wait_for_pause(AFTER_WAKE_APPLICATION_LOCK);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete succeeded while native application lock was reacquired");

    atomic_set(&pause_at, AFTER_WAIT_RETURN);
    resume_worker();
    wait_for_pause(AFTER_WAIT_RETURN);
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Wait returned without retaining application ownership");

    resume_worker();
    join_worker();
    zassert_equal(worker_release_result, OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle,
      test_delete_rejects_notifier_state_lock_without_application_owner)
{
    osal_id_t id;
    osal_id_t found_id;

    native_signal_calls = 0;
    zassert_equal(OS_CondVarCreate(&id, "notify-state", 0), OS_SUCCESS);
    start_worker(id, WORKER_SIGNAL, IN_NATIVE_SIGNAL);
    /* The application mutex is free here. Delete obtains its application
     * probe, then must fail promptly on the notifier's state_lock. */
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete ignored a notifier holding only state_lock");
    zassert_equal(OS_CondVarGetIdByName(&found_id, "notify-state"), OS_SUCCESS);
    zassert_equal(OS_ObjectIdToInteger(found_id), OS_ObjectIdToInteger(id),
                  "Delete changed the ID while state_lock was held");
    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(native_signal_calls, 1);
    zassert_equal(OS_CondVarDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_notifiers_complete_while_another_thread_owns_application_lock)
{
    osal_id_t id;
    osal_id_t found_id;

    native_signal_calls = 0;
    native_broadcast_calls = 0;
    zassert_equal(OS_CondVarCreate(&id, "notify-unowned", 0), OS_SUCCESS);
    zassert_equal(OS_CondVarLock(id), OS_SUCCESS);
    start_worker(id, WORKER_SIGNAL, IN_NATIVE_SIGNAL);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete waited for a notifier holding state_lock");
    zassert_equal(OS_CondVarGetIdByName(&found_id, "notify-unowned"), OS_SUCCESS);
    zassert_equal(OS_ObjectIdToInteger(found_id), OS_ObjectIdToInteger(id),
                  "Delete changed the ID while native Signal was paused");
    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    start_worker(id, WORKER_BROADCAST, IN_NATIVE_BROADCAST);
    zassert_equal(OS_CondVarDelete(id), OS_ERROR,
                  "Delete waited for a notifier holding state_lock");
    zassert_equal(OS_CondVarGetIdByName(&found_id, "notify-unowned"), OS_SUCCESS);
    zassert_equal(OS_ObjectIdToInteger(found_id), OS_ObjectIdToInteger(id),
                  "Delete changed the ID while native Broadcast was paused");
    resume_worker();
    join_worker();
    zassert_equal(worker_result, OS_SUCCESS);
    zassert_equal(native_signal_calls, 1);
    zassert_equal(native_broadcast_calls, 1);
    zassert_equal(OS_CondVarUnlock(id), OS_SUCCESS);
    zassert_equal(OS_CondVarDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_condvar_lifecycle, test_stop_after_validation_leaves_no_reservation)
{
    osal_id_t old_id;
    osal_id_t new_id;

    zassert_equal(OS_CondVarCreate(&old_id, "abort-old", 0), OS_SUCCESS);
    start_worker(old_id, WORKER_LOCK, AFTER_VALIDATION);
    stop_worker_after_validation();
    zassert_equal(OS_CondVarDelete(old_id), OS_SUCCESS);
    zassert_equal(OS_CondVarCreate(&new_id, "abort-new", 0), OS_SUCCESS);
    assert_same_slot_new_generation(old_id, new_id);
    zassert_equal(OS_CondVarDelete(new_id), OS_SUCCESS);
}

static void *suite_setup(void)
{
    zassert_equal(OS_API_Init(), OS_SUCCESS);
    return NULL;
}

ZTEST_SUITE(osal_zephyr_condvar_lifecycle, NULL, suite_setup, NULL, NULL, NULL);
