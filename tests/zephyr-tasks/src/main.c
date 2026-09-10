/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "os-impl-countsem.h"
#include "os-shared-idmap.h"
#include "osapi.h"

#define WATCHDOG_MS 3000
#define STACK_SIZE 2048
#define REUSE_ROUNDS 8

BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "native wait observations require UP scheduling");
BUILD_ASSERT(CONFIG_ZTEST_THREAD_PRIORITY == 2, "the controller must run at preempt priority 2");
BUILD_ASSERT(CONFIG_NUM_PREEMPT_PRIORITIES >= 4, "priority ordering requires at least four preempt priorities");

struct task_probe
{
    struct k_sem started;
    struct k_sem release;
    osal_id_t id;
    k_tid_t tid;
    int native_priority;
    void *custom_data;
    int32 status;
};

static struct task_probe identity_probe;
static struct task_probe high_probe;
static struct task_probe middle_probe;
static struct task_probe low_probe;
static struct task_probe exit_probe;
static struct task_probe reuse_probe;
static struct task_probe countsem_probe;
static struct task_probe table_lock_probe;
static struct k_sem observed_native_wait;
static struct k_sem shutdown_fallback_entered;
static struct k_sem reuse_wait;
static struct k_sem table_lock_delete_wait;
static atomic_ptr_t observed_sem;
static atomic_t fail_next_stack_allocation;
static atomic_t observe_shutdown_fallback;
static atomic_t successful_stack_frees;
static atomic_t delete_hook_calls;
static uintptr_t managed_sentinel;
static osal_id_t countsem_id = OS_OBJECT_ID_UNDEFINED;

int __real_z_impl_k_sem_take(struct k_sem *sem, k_timeout_t timeout);
k_thread_stack_t *__real_z_impl_k_thread_stack_alloc(size_t size, int flags);
int __real_z_impl_k_thread_stack_free(k_thread_stack_t *stack);
int32 __real_OS_ObjectIdGetById(OS_lock_mode_t lock_mode, osal_objtype_t obj_type, osal_id_t id,
                                OS_object_token_t *token);

int32 __wrap_OS_ObjectIdGetById(OS_lock_mode_t lock_mode, osal_objtype_t obj_type, osal_id_t id,
                                OS_object_token_t *token)
{
    if (atomic_get(&observe_shutdown_fallback) != 0 && k_current_get() == exit_probe.tid &&
        lock_mode == OS_LOCK_MODE_EXCLUSIVE && obj_type == OS_OBJECT_TYPE_OS_TASK)
    {
        atomic_set(&observe_shutdown_fallback, 0);
        k_sem_give(&shutdown_fallback_entered);
    }
    return __real_OS_ObjectIdGetById(lock_mode, obj_type, id, token);
}

int __wrap_z_impl_k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
    if (sem == atomic_ptr_get(&observed_sem))
    {
        k_sem_give(&observed_native_wait);
    }
    return __real_z_impl_k_sem_take(sem, timeout);
}

k_thread_stack_t *__wrap_z_impl_k_thread_stack_alloc(size_t size, int flags)
{
    if (atomic_cas(&fail_next_stack_allocation, 1, 0))
    {
        return NULL;
    }
    return __real_z_impl_k_thread_stack_alloc(size, flags);
}

int __wrap_z_impl_k_thread_stack_free(k_thread_stack_t *stack)
{
    int status = __real_z_impl_k_thread_stack_free(stack);

    if (status == 0)
    {
        atomic_inc(&successful_stack_frees);
    }
    return status;
}

static bool same_id(osal_id_t left, osal_id_t right)
{
    return OS_ObjectIdToInteger(left) == OS_ObjectIdToInteger(right);
}

static int slot_for_id(osal_id_t id)
{
    osal_index_t index;

    zassert_equal(OS_ConvertToArrayIndex(id, &index), OS_SUCCESS);
    zassert_true(index < OS_MAX_TASKS);
    return (int)index;
}

static void reset_probe(struct task_probe *probe)
{
    k_sem_reset(&probe->started);
    k_sem_reset(&probe->release);
    probe->id = OS_OBJECT_ID_UNDEFINED;
    probe->tid = NULL;
    probe->native_priority = INT32_MIN;
    probe->custom_data = NULL;
    probe->status = OS_ERROR;
}

static void capture_probe(struct task_probe *probe)
{
    k_thread_custom_data_set(&managed_sentinel);
    probe->id = OS_TaskGetId();
    probe->tid = k_current_get();
    probe->native_priority = k_thread_priority_get(probe->tid);
    probe->custom_data = k_thread_custom_data_get();
}

static void capture_and_wait(struct task_probe *probe)
{
    capture_probe(probe);
    k_sem_give(&probe->started);
    probe->status = k_sem_take(&probe->release, K_FOREVER);
}

static void identity_entry(void) { capture_and_wait(&identity_probe); }

static void high_entry(void) { capture_and_wait(&high_probe); }

static void middle_entry(void) { capture_and_wait(&middle_probe); }

static void low_entry(void) { capture_and_wait(&low_probe); }

static void natural_return_entry(void)
{
    capture_probe(&exit_probe);
    k_sem_give(&exit_probe.started);
}

static void explicit_exit_entry(void)
{
    capture_probe(&exit_probe);
    exit_probe.status = OS_TaskDelete(exit_probe.id);
    k_sem_give(&exit_probe.started);
    OS_TaskExit();
}

static void shutdown_return_entry(void) { capture_and_wait(&exit_probe); }

static void reuse_entry(void)
{
    capture_probe(&reuse_probe);
    k_sem_take(&reuse_wait, K_FOREVER);
}

static void countsem_waiter_entry(void)
{
    capture_probe(&countsem_probe);
    countsem_probe.status = OS_CountSemTake(countsem_id);
    k_sem_give(&countsem_probe.started);
    OS_TaskExit();
}

static void delete_hook(void) { atomic_inc(&delete_hook_calls); }

static void table_lock_entry(void)
{
    capture_probe(&table_lock_probe);
    table_lock_probe.status = OS_TaskInstallDeleteHandler(delete_hook);
    OS_Lock_Global_Impl(OS_OBJECT_TYPE_OS_BINSEM);
    k_sem_give(&table_lock_probe.started);
    k_sem_take(&table_lock_probe.release, K_FOREVER);
    OS_Unlock_Global_Impl(OS_OBJECT_TYPE_OS_BINSEM);
    k_sem_take(&table_lock_delete_wait, K_FOREVER);
}

static void wait_started(struct task_probe *probe)
{
    zassert_equal(k_sem_take(&probe->started, K_MSEC(WATCHDOG_MS)), 0, "managed task did not signal startup");
}

static void wait_native_take(void)
{
    zassert_equal(k_sem_take(&observed_native_wait, K_MSEC(WATCHDOG_MS)), 0,
                  "managed task did not reach the native wait");
}

static void assert_name_gone(const char *name)
{
    osal_id_t found;
    int32 status = OS_SUCCESS;
    int64_t deadline = k_uptime_get() + WATCHDOG_MS;

    do
    {
        status = OS_TaskGetIdByName(&found, name);
        if (status != OS_SUCCESS)
        {
            break;
        }
        k_sleep(K_MSEC(1));
    } while (k_uptime_get() < deadline);

    zassert_equal(status, OS_ERR_NAME_NOT_FOUND, "%s remained registered: %d", name, (int)status);
}

static osal_id_t create_task(const char *name, osal_task_entry entry, osal_priority_t priority)
{
    osal_id_t id;
    int32 status;

    status = OS_TaskCreate(&id, name, entry, OSAL_TASK_STACK_ALLOCATE, STACK_SIZE, priority, 0);
    zassert_equal(status, OS_SUCCESS, "Create %s returned %d", name, (int)status);
    return id;
}

ZTEST(osal_zephyr_tasks, test_public_identity_info_name_and_system_data)
{
    static uintptr_t external_sentinel;
    osal_id_t id;
    osal_id_t found;
    OS_task_prop_t info;
    void *saved_custom_data;

    reset_probe(&identity_probe);
    saved_custom_data = k_thread_custom_data_get();
    k_thread_custom_data_set(&external_sentinel);
    zassert_false(OS_ObjectIdDefined(OS_TaskGetId()), "external ztest thread acquired an OSAL ID");
    zassert_equal(k_thread_custom_data_get(), &external_sentinel, "external GetId changed custom data");

    id = create_task("identity", identity_entry, OSAL_PRIORITY_C(0));
    wait_started(&identity_probe);

    zassert_true(same_id(identity_probe.id, id), "managed GetId returned another ID");
    zassert_not_null(identity_probe.tid);
    zassert_equal(identity_probe.custom_data, &managed_sentinel, "managed GetId changed custom data");
    zassert_equal(OS_TaskGetIdByName(&found, "identity"), OS_SUCCESS);
    zassert_true(same_id(found, id));
    zassert_equal(OS_TaskGetInfo(id, &info), OS_SUCCESS);
    zassert_mem_equal(info.name, "identity", sizeof("identity"));
    zassert_false(OS_ObjectIdDefined(info.creator));
    zassert_equal(info.stack_size, STACK_SIZE);
    zassert_equal(info.priority, OSAL_PRIORITY_C(0));
    zassert_equal(OS_TaskFindIdBySystemData(&found, &identity_probe.tid, sizeof(identity_probe.tid)), OS_SUCCESS);
    zassert_true(same_id(found, id));
    zassert_equal(OS_TaskFindIdBySystemData(&found, NULL, sizeof(identity_probe.tid)), OS_INVALID_POINTER);
    zassert_equal(OS_TaskFindIdBySystemData(&found, &identity_probe.tid, 0), OS_INVALID_POINTER);

    k_sem_give(&identity_probe.release);
    assert_name_gone("identity");
    k_thread_custom_data_set(saved_custom_data);
}

ZTEST(osal_zephyr_tasks, test_priority_endpoints_ordering_and_updates)
{
    osal_id_t high_id;
    osal_id_t middle_id;
    osal_id_t low_id;
    OS_task_prop_t info;

    reset_probe(&high_probe);
    reset_probe(&middle_probe);
    reset_probe(&low_probe);
    high_id = create_task("priority-high", high_entry, OSAL_PRIORITY_C(0));
    middle_id = create_task("priority-middle", middle_entry, OSAL_PRIORITY_C(128));
    low_id = create_task("priority-low", low_entry, OSAL_PRIORITY_C(255));
    wait_started(&high_probe);
    wait_started(&middle_probe);
    wait_started(&low_probe);

    zassert_equal(high_probe.native_priority, K_PRIO_PREEMPT(0));
    zassert_equal(low_probe.native_priority, K_PRIO_PREEMPT(CONFIG_NUM_PREEMPT_PRIORITIES - 1));
    zassert_true(high_probe.native_priority < middle_probe.native_priority);
    zassert_true(middle_probe.native_priority < low_probe.native_priority);

    zassert_equal(OS_TaskSetPriority(middle_id, OSAL_PRIORITY_C(0)), OS_SUCCESS);
    zassert_equal(k_thread_priority_get(middle_probe.tid), K_PRIO_PREEMPT(0));
    zassert_equal(OS_TaskGetInfo(middle_id, &info), OS_SUCCESS);
    zassert_equal(info.priority, OSAL_PRIORITY_C(0));
    zassert_equal(OS_TaskSetPriority(middle_id, OSAL_PRIORITY_C(255)), OS_SUCCESS);
    zassert_equal(k_thread_priority_get(middle_probe.tid), K_PRIO_PREEMPT(CONFIG_NUM_PREEMPT_PRIORITIES - 1));
    zassert_equal(OS_TaskGetInfo(middle_id, &info), OS_SUCCESS);
    zassert_equal(info.priority, OSAL_PRIORITY_C(255));

    k_sem_give(&high_probe.release);
    k_sem_give(&middle_probe.release);
    k_sem_give(&low_probe.release);
    assert_name_gone("priority-high");
    assert_name_gone("priority-middle");
    assert_name_gone("priority-low");
    ARG_UNUSED(high_id);
    ARG_UNUSED(low_id);
}

ZTEST(osal_zephyr_tasks, test_entry_return_and_explicit_exit_cleanup)
{
    osal_id_t id;

    reset_probe(&exit_probe);
    id = create_task("natural-return", natural_return_entry, OSAL_PRIORITY_C(0));
    wait_started(&exit_probe);
    zassert_true(same_id(exit_probe.id, id));
    assert_name_gone("natural-return");
    zassert_equal(OS_TaskGetInfo(id, &(OS_task_prop_t){0}), OS_ERR_INVALID_ID);

    reset_probe(&exit_probe);
    id = create_task("explicit-exit", explicit_exit_entry, OSAL_PRIORITY_C(0));
    wait_started(&exit_probe);
    zassert_true(same_id(exit_probe.id, id));
    zassert_equal(exit_probe.status, OS_ERROR, "a managed task deleted itself through OS_TaskDelete");
    assert_name_gone("explicit-exit");
}

ZTEST(osal_zephyr_tasks, test_external_delete_waiter_and_repeated_slot_reuse)
{
    uintptr_t prior_id[OS_MAX_TASKS] = {0};
    bool seen[OS_MAX_TASKS] = {false};
    int reused_slots = 0;

    for (int round = 0; round < REUSE_ROUNDS; ++round)
    {
        osal_id_t id;
        int slot;

        reset_probe(&reuse_probe);
        k_sem_reset(&reuse_wait);
        k_sem_reset(&observed_native_wait);
        atomic_ptr_set(&observed_sem, &reuse_wait);
        id = create_task("delete-reuse", reuse_entry, OSAL_PRIORITY_C(0));
        wait_native_take();

        zassert_equal(OS_TaskDelete(id), OS_SUCCESS);
        assert_name_gone("delete-reuse");
        slot = slot_for_id(id);
        if (seen[slot])
        {
            zassert_not_equal(prior_id[slot], OS_ObjectIdToInteger(id));
            ++reused_slots;
        }
        seen[slot] = true;
        prior_id[slot] = OS_ObjectIdToInteger(id);
        atomic_ptr_set(&observed_sem, NULL);
    }
    zassert_true(reused_slots > 0, "repeated creation did not reuse a task slot");
}

ZTEST(osal_zephyr_tasks, test_managed_countsem_wait_rejects_delete)
{
    osal_id_t task_id;
    osal_id_t found;

    reset_probe(&countsem_probe);
    k_sem_reset(&observed_native_wait);
    zassert_equal(OS_CountSemCreate(&countsem_id, "task-wait-sem", 0, 0), OS_SUCCESS);
    atomic_ptr_set(&observed_sem, &OS_impl_count_sem_table[0].sem);
    task_id = create_task("countsem-waiter", countsem_waiter_entry, OSAL_PRIORITY_C(0));
    wait_native_take();

    zassert_equal(OS_TaskDelete(task_id), OS_ERROR);
    zassert_equal(OS_TaskGetIdByName(&found, "countsem-waiter"), OS_SUCCESS);
    zassert_true(same_id(found, task_id), "failed delete did not restore the public ID");
    zassert_equal(OS_CountSemGive(countsem_id), OS_SUCCESS);
    wait_started(&countsem_probe);
    zassert_equal(countsem_probe.status, OS_SUCCESS);
    assert_name_gone("countsem-waiter");
    atomic_ptr_set(&observed_sem, NULL);
    zassert_equal(OS_CountSemDelete(countsem_id), OS_SUCCESS, "managed wait left an admitted semaphore user behind");
    countsem_id = OS_OBJECT_ID_UNDEFINED;
}

ZTEST(osal_zephyr_tasks, test_table_lock_rejects_delete_and_defers_hook)
{
    osal_id_t task_id;
    osal_id_t found;
    OS_task_prop_t info;

    reset_probe(&table_lock_probe);
    k_sem_reset(&table_lock_delete_wait);
    k_sem_reset(&observed_native_wait);
    atomic_set(&delete_hook_calls, 0);
    atomic_ptr_set(&observed_sem, &table_lock_delete_wait);
    task_id = create_task("table-lock", table_lock_entry, OSAL_PRIORITY_C(0));
    wait_started(&table_lock_probe);
    zassert_equal(table_lock_probe.status, OS_SUCCESS, "managed task could not install its delete hook");

    zassert_equal(OS_TaskDelete(task_id), OS_ERROR);
    zassert_equal(atomic_get(&delete_hook_calls), 0, "failed deletion called the delete hook");
    zassert_equal(OS_TaskGetInfo(task_id, &info), OS_SUCCESS);
    zassert_equal(OS_TaskGetIdByName(&found, "table-lock"), OS_SUCCESS);
    zassert_true(same_id(found, task_id), "failed delete did not restore the public ID");

    k_sem_give(&table_lock_probe.release);
    wait_native_take();
    zassert_equal(OS_TaskDelete(task_id), OS_SUCCESS);
    zassert_equal(atomic_get(&delete_hook_calls), 1, "successful deletion did not call the delete hook exactly once");
    assert_name_gone("table-lock");
    atomic_ptr_set(&observed_sem, NULL);
}

ZTEST(osal_zephyr_tasks, test_create_rejections_and_allocation_rollback)
{
    static uint8_t supplied_stack[STACK_SIZE];
    osal_id_t id;

    zassert_equal(
        OS_TaskCreate(&id, "zero-stack", natural_return_entry, OSAL_TASK_STACK_ALLOCATE, 0, OSAL_PRIORITY_C(128), 0),
        OS_ERR_INVALID_SIZE);
    zassert_equal(OS_TaskCreate(&id, "supplied-stack", natural_return_entry, supplied_stack, sizeof(supplied_stack),
                                OSAL_PRIORITY_C(128), 0),
                  OS_ERR_NOT_IMPLEMENTED);
    zassert_equal(OS_TaskCreate(&id, "unknown-flags", natural_return_entry, OSAL_TASK_STACK_ALLOCATE, STACK_SIZE,
                                OSAL_PRIORITY_C(128), UINT32_C(0x80000000)),
                  OS_ERR_NOT_IMPLEMENTED);

    atomic_set(&fail_next_stack_allocation, 1);
    zassert_equal(OS_TaskCreate(&id, "alloc-failure", natural_return_entry, OSAL_TASK_STACK_ALLOCATE, STACK_SIZE,
                                OSAL_PRIORITY_C(128), 0),
                  OS_ERROR);
    zassert_equal(OS_TaskGetIdByName(&id, "alloc-failure"), OS_ERR_NAME_NOT_FOUND);

    reset_probe(&exit_probe);
    id = create_task("alloc-failure", natural_return_entry, OSAL_PRIORITY_C(0));
    wait_started(&exit_probe);
    assert_name_gone("alloc-failure");
}

ZTEST(osal_zephyr_tasks, test_zzz_shutdown_exit_waits_for_reservation)
{
    OS_object_token_t token;
    osal_id_t task_id;
    int frees_before;
    int64_t deadline;

    reset_probe(&exit_probe);
    k_sem_reset(&shutdown_fallback_entered);
    atomic_set(&observe_shutdown_fallback, 0);
    task_id = create_task("shutdown-return", shutdown_return_entry, OSAL_PRIORITY_C(0));
    wait_started(&exit_probe);
    zassert_true(same_id(exit_probe.id, task_id));

    zassert_equal(OS_ObjectIdGetById(OS_LOCK_MODE_EXCLUSIVE, OS_OBJECT_TYPE_OS_TASK, task_id, &token), OS_SUCCESS);
    OS_ApplicationShutdown(true);
    frees_before = atomic_get(&successful_stack_frees);
    atomic_set(&observe_shutdown_fallback, 1);
    k_sem_give(&exit_probe.release);
    zassert_equal(k_sem_take(&shutdown_fallback_entered, K_MSEC(WATCHDOG_MS)), 0,
                  "exiting task did not enter shutdown fallback");

    zassert_equal(OS_ObjectIdFinalizeDelete(OS_ERROR, &token), OS_ERROR);
    atomic_set(&observe_shutdown_fallback, 0);

    deadline = k_uptime_get() + WATCHDOG_MS;
    while (atomic_get(&successful_stack_frees) == frees_before && k_uptime_get() < deadline)
    {
        k_sleep(K_MSEC(1));
    }
    zassert_true(atomic_get(&successful_stack_frees) > frees_before, "shutdown exit did not reclaim its dynamic stack");
    zassert_equal(OS_ObjectIdGetById(OS_LOCK_MODE_EXCLUSIVE, OS_OBJECT_TYPE_OS_TASK, task_id, &token),
                  OS_ERR_INVALID_ID, "shutdown exit left its public task ID registered");
}

static void *suite_setup(void)
{
    struct task_probe *probes[] = {&identity_probe, &high_probe,  &middle_probe,   &low_probe,
                                   &exit_probe,     &reuse_probe, &countsem_probe, &table_lock_probe};

    for (size_t i = 0; i < ARRAY_SIZE(probes); ++i)
    {
        k_sem_init(&probes[i]->started, 0, 1);
        k_sem_init(&probes[i]->release, 0, 1);
        probes[i]->id = OS_OBJECT_ID_UNDEFINED;
    }
    k_sem_init(&observed_native_wait, 0, 1);
    k_sem_init(&shutdown_fallback_entered, 0, 1);
    k_sem_init(&reuse_wait, 0, 1);
    k_sem_init(&table_lock_delete_wait, 0, 1);
    zassert_equal(OS_API_Init(), OS_SUCCESS);
    return NULL;
}

ZTEST_SUITE(osal_zephyr_tasks, NULL, suite_setup, NULL, NULL, NULL);
