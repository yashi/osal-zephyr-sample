/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/kernel/obj_core.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include <zephyr/tc_util.h>
#include <zephyr/ztest.h>

#include "osapi.h"
#include "os-impl-countsem.h"
#include "os-shared-idmap.h"

#define WATCHDOG_MS 5000
#define REPEAT_COUNT 8
#define WORKER_COUNT 3

BUILD_ASSERT(OS_MAX_COUNT_SEMAPHORES == 3,
             "this fixture requires three counting semaphore slots");
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "this fixture requires UP scheduling");
BUILD_ASSERT(CONFIG_ZTEST_THREAD_PRIORITY == 2,
             "controller priority must be 2");
BUILD_ASSERT(IS_ENABLED(CONFIG_OBJ_CORE) && IS_ENABLED(CONFIG_OBJ_CORE_SEM),
             "object-core semaphore registration is required");
BUILD_ASSERT(IS_ENABLED(CONFIG_SYS_CLOCK_EXISTS), "finite waits require a clock");
BUILD_ASSERT(IS_ENABLED(CONFIG_TIMEOUT_64BIT), "finite UINT32_MAX is covered");

enum pause_point {
    PAUSE_NONE,
    AFTER_VALIDATION,
    BEFORE_STATE_LOCK,
    BEFORE_NATIVE_TAKE,
    AFTER_NATIVE_TAKE,
    IN_NATIVE_GIVE
};

enum operation {
    WORK_GIVE,
    WORK_TAKE,
    WORK_TIMED_WAIT
};

struct worker_context {
    struct k_thread thread;
    k_tid_t tid;
    osal_id_t id;
    enum operation operation;
    uint32 timeout_ms;
    int32 result;
    bool started;
    bool capture_native_timeout;
    k_ticks_t native_timeout_ticks;
    int64_t native_entry_ms;
    struct k_sem reached;
    struct k_sem resume;
    struct k_sem native_started;
    struct k_sem done;
};

K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, WORKER_COUNT, 2048);
static struct worker_context workers[WORKER_COUNT];
static atomic_t pause_at;

int32 __real_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token);
int __real_z_impl_k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);
int __real_z_impl_k_sem_take(struct k_sem *sem, k_timeout_t timeout);
void __real_z_impl_k_sem_give(struct k_sem *sem);

static struct worker_context *current_worker(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(workers); ++i) {
        if (workers[i].started && k_current_get() == &workers[i].thread) {
            return &workers[i];
        }
    }
    return NULL;
}

static int slot_for_id(osal_id_t id)
{
    osal_index_t index;

    zassert_equal(OS_ConvertToArrayIndex(id, &index), OS_SUCCESS);
    zassert_true(index < OS_MAX_COUNT_SEMAPHORES);
    return (int)index;
}

static bool state_lock_for_worker(const struct worker_context *ctx,
                                  struct k_mutex *lock)
{
    return lock == &OS_impl_count_sem_table[slot_for_id(ctx->id)].lock;
}

static bool native_sem_for_worker(const struct worker_context *ctx,
                                  struct k_sem *sem)
{
    return sem == &OS_impl_count_sem_table[slot_for_id(ctx->id)].sem;
}

static void gate(struct worker_context *ctx, enum pause_point point)
{
    if (ctx != NULL && atomic_cas(&pause_at, point, PAUSE_NONE)) {
        k_sem_give(&ctx->reached);
        zassert_equal(k_sem_take(&ctx->resume, K_MSEC(WATCHDOG_MS)), 0,
                      "worker did not receive release at gate %d", point);
    }
}

/* Successful shared validation is observed, never modified. */
int32 __wrap_OS_ObjectIdGetById(OS_lock_mode_t mode, osal_objtype_t type,
                                osal_id_t id, OS_object_token_t *token)
{
    struct worker_context *ctx = current_worker();
    int32 status = __real_OS_ObjectIdGetById(mode, type, id, token);

    if (status == OS_SUCCESS && type == OS_OBJECT_TYPE_OS_COUNTSEM &&
        mode == OS_LOCK_MODE_NONE) {
        gate(ctx, AFTER_VALIDATION);
    }
    return status;
}

int __wrap_z_impl_k_mutex_lock(struct k_mutex *lock, k_timeout_t timeout)
{
    struct worker_context *ctx = current_worker();

    if (ctx != NULL && state_lock_for_worker(ctx, lock)) {
        gate(ctx, BEFORE_STATE_LOCK);
    }
    return __real_z_impl_k_mutex_lock(lock, timeout);
}

int __wrap_z_impl_k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
    struct worker_context *ctx = current_worker();
    int status;

    if (ctx != NULL && native_sem_for_worker(ctx, sem)) {
        if (ctx->capture_native_timeout) {
            ctx->native_timeout_ticks = timeout.ticks;
            ctx->native_entry_ms = k_uptime_get();
        }
        gate(ctx, BEFORE_NATIVE_TAKE);
        /* This is immediately before the real kernel call. */
        k_sem_give(&ctx->native_started);
        status = __real_z_impl_k_sem_take(sem, timeout);
        gate(ctx, AFTER_NATIVE_TAKE);
        return status;
    }
    return __real_z_impl_k_sem_take(sem, timeout);
}

void __wrap_z_impl_k_sem_give(struct k_sem *sem)
{
    struct worker_context *ctx = current_worker();

    if (ctx != NULL && native_sem_for_worker(ctx, sem)) {
        gate(ctx, IN_NATIVE_GIVE);
    }
    __real_z_impl_k_sem_give(sem);
}

static void worker_entry(void *arg1, void *arg2, void *arg3)
{
    struct worker_context *ctx = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    if (ctx->operation == WORK_GIVE) {
        ctx->result = OS_CountSemGive(ctx->id);
    } else if (ctx->operation == WORK_TAKE) {
        ctx->result = OS_CountSemTake(ctx->id);
    } else {
        ctx->result = OS_CountSemTimedWait(ctx->id, ctx->timeout_ms);
    }
    k_sem_give(&ctx->done);
}

static void start_worker(int number, osal_id_t id, enum operation operation,
                         uint32 timeout_ms, enum pause_point point)
{
    struct worker_context *ctx = &workers[number];

    zassert_false(ctx->started, "worker %d is already live", number);
    k_sem_reset(&ctx->reached);
    k_sem_reset(&ctx->resume);
    k_sem_reset(&ctx->native_started);
    k_sem_reset(&ctx->done);
    ctx->id = id;
    ctx->operation = operation;
    ctx->timeout_ms = timeout_ms;
    ctx->result = OS_ERROR;
    ctx->capture_native_timeout = false;
    ctx->native_timeout_ticks = 0;
    ctx->native_entry_ms = 0;
    atomic_set(&pause_at, point);
    ctx->started = true;
    ctx->tid = k_thread_create(&ctx->thread, worker_stacks[number],
                               K_THREAD_STACK_SIZEOF(worker_stacks[number]),
                               worker_entry, ctx, NULL, NULL,
                               K_PRIO_PREEMPT(1), 0, K_FOREVER);
    k_thread_start(ctx->tid);
    if (point != PAUSE_NONE) {
        zassert_equal(k_sem_take(&ctx->reached, K_MSEC(WATCHDOG_MS)), 0,
                      "worker %d missed gate %d", number, point);
    }
}

static void release_worker(int number)
{
    k_sem_give(&workers[number].resume);
}

static void release_into_native_take(int number)
{
    struct worker_context *ctx = &workers[number];

    release_worker(number);
    zassert_equal(k_sem_take(&ctx->native_started, K_MSEC(WATCHDOG_MS)), 0,
                  "worker %d did not enter real native take boundary", number);
    /* On UP the priority-1 worker continues into the real Take and blocks
     * before this priority-2 controller resumes. The controller owns no
     * provider mutex here, so priority inheritance cannot reverse that order. */
}

static void join_worker(int number)
{
    struct worker_context *ctx = &workers[number];

    zassert_equal(k_sem_take(&ctx->done, K_MSEC(WATCHDOG_MS)), 0,
                  "worker %d did not complete", number);
    zassert_equal(k_thread_join(&ctx->thread, K_MSEC(WATCHDOG_MS)), 0,
                  "worker %d did not terminate", number);
    ctx->started = false;
}

static void abort_after_shared_validation(int number)
{
    struct worker_context *ctx = &workers[number];

    zassert_true(ctx->started);
    k_thread_abort(&ctx->thread);
    zassert_equal(k_thread_join(&ctx->thread, K_MSEC(WATCHDOG_MS)), 0);
    ctx->started = false;
}

static void assert_same_slot_new_id(osal_id_t old_id, osal_id_t new_id)
{
    zassert_not_equal(OS_ObjectIdToInteger(old_id), OS_ObjectIdToInteger(new_id));
    zassert_equal(slot_for_id(old_id), slot_for_id(new_id));
}

static int32 count_of(osal_id_t id)
{
    OS_count_sem_prop_t info;

    zassert_equal(OS_CountSemGetInfo(id, &info), OS_SUCCESS);
    return info.value;
}

static void assert_live_id(osal_id_t id, const char *name)
{
    osal_id_t found;

    zassert_equal(OS_CountSemGetIdByName(&found, name), OS_SUCCESS);
    zassert_equal(OS_ObjectIdToInteger(found), OS_ObjectIdToInteger(id));
}

static void create(osal_id_t *id, const char *name, uint32 initial)
{
    int32 status = OS_CountSemCreate(id, name, initial, 0);

    zassert_equal(status, OS_SUCCESS, "Create %s returned %d", name, (int)status);
}

static void stale_operation(enum operation operation, enum pause_point point,
                            uint32 timeout_ms, const char *old_name,
                            const char *new_name)
{
    osal_id_t old_id;
    osal_id_t new_id;
    osal_id_t neighbours[2];

    create(&old_id, old_name, 0);
    /* The two distinct names occupy every other physical slot, so a later
     * Create can only reuse old_id's slot. */
    create(&neighbours[0], "stale-neighbour-a", 0);
    create(&neighbours[1], "stale-neighbour-b", 0);
    start_worker(0, old_id, operation, timeout_ms, point);
    zassert_equal(OS_CountSemDelete(old_id), OS_SUCCESS);
    /* Two real tokens make an erroneous old-generation Take observable
     * without turning this regression into an indefinite blocked wait. */
    create(&new_id, new_name, 2);
    assert_same_slot_new_id(old_id, new_id);
    release_worker(0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_ERR_INVALID_ID);
    zassert_equal(count_of(new_id), 2, "stale operation changed replacement");
    zassert_equal(OS_CountSemDelete(new_id), OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(neighbours[0]), OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(neighbours[1]), OS_SUCCESS);
}

struct walk_state {
    int callbacks;
    int provider_count;
    bool seen[OS_MAX_COUNT_SEMAPHORES];
};

static int count_provider_sem(struct k_obj_core *core, void *data)
{
    struct walk_state *state = data;

    /* Other static and test semaphores share this kernel object type.  Bound
     * the complete walk so corruption cannot produce an unbounded traversal. */
    if (++state->callbacks > 512) {
        return 1; /* A cycle or duplicate must not turn the test into a hang. */
    }
    for (int i = 0; i < OS_MAX_COUNT_SEMAPHORES; ++i) {
        if (core == K_OBJ_CORE(&OS_impl_count_sem_table[i].sem)) {
            ++state->provider_count;
            if (state->seen[i]) {
                return 1;
            }
            state->seen[i] = true;
            break;
        }
    }
    return 0;
}

static void assert_registered_set(const bool expected[OS_MAX_COUNT_SEMAPHORES])
{
    struct walk_state state = {0};
    struct k_obj_type *type = k_obj_type_find(K_OBJ_TYPE_SEM_ID);

    zassert_not_null(type);
    zassert_equal(k_obj_type_walk_locked(type, count_provider_sem, &state), 0,
                  "object core walk was terminated");
    int expected_count = 0;
    for (int i = 0; i < OS_MAX_COUNT_SEMAPHORES; ++i) {
        zassert_equal(state.seen[i], expected[i], "provider sem slot %d", i);
        expected_count += expected[i] ? 1 : 0;
    }
    zassert_equal(state.provider_count, expected_count,
                  "walk did not find the expected live provider objects");
}

ZTEST(osal_zephyr_countsem, test_public_basic_limits_names_and_exhaustion)
{
    osal_id_t ids[3];
    osal_id_t found;
    OS_count_sem_prop_t info;

    zassert_equal(OS_CountSemCreate(NULL, "null-id", 0, 0), OS_INVALID_POINTER);
    zassert_equal(OS_CountSemCreate(&found, NULL, 0, 0), OS_INVALID_POINTER);
    zassert_equal(OS_CountSemCreate(&found, "bad-value", (uint32)INT32_MAX + 1U, 0),
                  OS_INVALID_SEM_VALUE);
    zassert_equal(OS_CountSemCreate(&found, "bad-max", UINT32_MAX, 0), OS_INVALID_SEM_VALUE);
    create(&ids[0], "count-basic", 1);
    zassert_equal(OS_CountSemCreate(&found, "count-basic", 0, 0), OS_ERR_NAME_TAKEN);
    zassert_equal(OS_CountSemGetInfo(ids[0], NULL), OS_INVALID_POINTER);
    zassert_equal(OS_CountSemGetIdByName(NULL, "count-basic"), OS_INVALID_POINTER);
    zassert_equal(OS_CountSemGetIdByName(&found, NULL), OS_INVALID_POINTER);
    assert_live_id(ids[0], "count-basic");
    zassert_equal(OS_CountSemGetInfo(ids[0], &info), OS_SUCCESS);
    zassert_equal(info.value, 1);
    zassert_equal(strcmp(info.name, "count-basic"), 0);
    zassert_equal(OS_CountSemTake(ids[0]), OS_SUCCESS);
    zassert_equal(OS_CountSemTimedWait(ids[0], 0), OS_SEM_TIMEOUT);
    zassert_equal(OS_CountSemGive(ids[0]), OS_SUCCESS);
    zassert_equal(OS_CountSemTimedWait(ids[0], 0), OS_SUCCESS);
    create(&ids[1], "count-neighbor-a", 0);
    create(&ids[2], "count-neighbor-b", 0);
    zassert_equal(OS_CountSemCreate(&found, "count-full", 0, 0), OS_ERR_NO_FREE_IDS);
    for (int i = 0; i < 3; ++i) {
        zassert_equal(OS_CountSemDelete(ids[i]), OS_SUCCESS);
    }
    zassert_equal(OS_CountSemGive(ids[0]), OS_ERR_INVALID_ID);
    zassert_equal(OS_CountSemTake(ids[0]), OS_ERR_INVALID_ID);
    zassert_equal(OS_CountSemTimedWait(ids[0], 0), OS_ERR_INVALID_ID);
    zassert_equal(OS_CountSemDelete(ids[0]), OS_ERR_INVALID_ID);
}

ZTEST(osal_zephyr_countsem, test_maximum_overflow_competing_givers_and_take_race)
{
    osal_id_t id;
    int32 expected;

    create(&id, "count-max", INT32_MAX);
    zassert_equal(count_of(id), INT32_MAX);
    zassert_equal(OS_CountSemGive(id), OS_SEM_FAILURE);
    zassert_equal(count_of(id), INT32_MAX);
    zassert_equal(OS_CountSemTake(id), OS_SUCCESS);
    zassert_equal(OS_CountSemGive(id), OS_SUCCESS);
    zassert_equal(count_of(id), INT32_MAX);
    zassert_equal(OS_CountSemTake(id), OS_SUCCESS); /* max - 1 */
    /* The first real Give holds provider state at native Give. The second is
     * released from its pre-lock gate and blocks on that same real mutex. */
    start_worker(0, id, WORK_GIVE, 0, IN_NATIVE_GIVE);
    start_worker(1, id, WORK_GIVE, 0, BEFORE_STATE_LOCK);
    release_worker(1);
    /* Worker 1 outranks this controller and blocks on worker 0's state
     * mutex before the controller can release its paused owner. */
    release_worker(0);
    join_worker(0);
    join_worker(1);
    zassert_true((workers[0].result == OS_SUCCESS && workers[1].result == OS_SEM_FAILURE) ||
                 (workers[1].result == OS_SUCCESS && workers[0].result == OS_SEM_FAILURE));
    zassert_equal(count_of(id), INT32_MAX);
    /* Take is admitted but cannot touch native state. Give has made its
     * non-full decision and holds state at native Give. Take completes
     * without state ownership, then the real Give observes that decrement. */
    zassert_equal(OS_CountSemTake(id), OS_SUCCESS); /* max - 1 */
    start_worker(0, id, WORK_TAKE, 0, BEFORE_NATIVE_TAKE);
    start_worker(1, id, WORK_GIVE, 0, IN_NATIVE_GIVE);
    release_worker(0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SUCCESS);
    release_worker(1);
    join_worker(1);
    zassert_equal(workers[1].result, OS_SUCCESS);
    expected = INT32_MAX - 2 + (workers[1].result == OS_SUCCESS ? 1 : 0);
    zassert_equal(count_of(id), expected);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_countsem, test_stale_none_calls_before_provider_admission)
{
    stale_operation(WORK_GIVE, AFTER_VALIDATION, 0, "stale-give-a", "stale-give-b");
    stale_operation(WORK_GIVE, BEFORE_STATE_LOCK, 0, "stale-give-c", "stale-give-d");
    stale_operation(WORK_TAKE, AFTER_VALIDATION, 0, "stale-take-a", "stale-take-b");
    stale_operation(WORK_TAKE, BEFORE_STATE_LOCK, 0, "stale-take-c", "stale-take-d");
    stale_operation(WORK_TIMED_WAIT, AFTER_VALIDATION, 40, "stale-time-a", "stale-time-b");
    stale_operation(WORK_TIMED_WAIT, BEFORE_STATE_LOCK, 40, "stale-time-c", "stale-time-d");
    for (int i = 0; i < REPEAT_COUNT; ++i) {
        stale_operation(WORK_GIVE, BEFORE_STATE_LOCK, 0, "stale-repeat-a", "stale-repeat-b");
    }
}

ZTEST(osal_zephyr_countsem, test_admission_native_wait_and_atomic_release_quiesce_delete)
{
    osal_id_t id;
    osal_id_t replacement;
    osal_id_t neighbours[2];

    create(&id, "admission-native", 0);
    create(&neighbours[0], "admit-neighbor-a", 0);
    create(&neighbours[1], "admit-neighbor-b", 0);
    start_worker(0, id, WORK_TAKE, 0, BEFORE_NATIVE_TAKE);
    zassert_equal(OS_CountSemDelete(id), OS_SEM_FAILURE);
    assert_live_id(id, "admission-native");
    zassert_equal(OS_CountSemGive(id), OS_SUCCESS);
    release_worker(0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);
    create(&replacement, "admission-reuse", 0);
    assert_same_slot_new_id(id, replacement);
    zassert_equal(OS_CountSemDelete(replacement), OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(neighbours[0]), OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(neighbours[1]), OS_SUCCESS);

    create(&id, "admit-timed", 0);
    start_worker(0, id, WORK_TIMED_WAIT, 100, BEFORE_NATIVE_TAKE);
    zassert_equal(OS_CountSemDelete(id), OS_SEM_FAILURE);
    zassert_equal(OS_CountSemGive(id), OS_SUCCESS);
    release_worker(0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);

    create(&id, "after-success", 1);
    start_worker(0, id, WORK_TAKE, 0, AFTER_NATIVE_TAKE);
    zassert_equal(OS_CountSemDelete(id), OS_SEM_FAILURE);
    release_worker(0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);

    create(&id, "after-timeout", 0);
    start_worker(0, id, WORK_TIMED_WAIT, 20, AFTER_NATIVE_TAKE);
    zassert_equal(OS_CountSemDelete(id), OS_SEM_FAILURE);
    release_worker(0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SEM_TIMEOUT);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_countsem, test_native_give_holds_state_and_preserves_id)
{
    osal_id_t id;

    create(&id, "native-give", 0);
    start_worker(0, id, WORK_GIVE, 0, IN_NATIVE_GIVE);
    zassert_equal(OS_CountSemDelete(id), OS_SEM_FAILURE);
    assert_live_id(id, "native-give");
    release_worker(0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SUCCESS);
    zassert_equal(count_of(id), 1);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_countsem, test_timeout_budget_and_available_uint32_timeout)
{
    osal_id_t id;
    int64_t start;
    int64_t elapsed;
    int64_t tick_tolerance_ms =
        3 * DIV_ROUND_UP(1000, CONFIG_SYS_CLOCK_TICKS_PER_SEC);
    uint64_t requested_ticks = k_ms_to_ticks_ceil64(90);
    uint64_t elapsed_to_native_ticks;
    uint64_t minimum_remaining_ticks;

    create(&id, "timeout-lock", 0);
    zassert_equal(k_mutex_lock(&OS_impl_count_sem_table[slot_for_id(id)].lock, K_FOREVER), 0);
    start_worker(0, id, WORK_TIMED_WAIT, 0, PAUSE_NONE);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SEM_TIMEOUT);
    start = k_uptime_get();
    start_worker(0, id, WORK_TIMED_WAIT, 20, PAUSE_NONE);
    join_worker(0);
    elapsed = k_uptime_get() - start;
    zassert_equal(workers[0].result, OS_SEM_TIMEOUT);
    zassert_true(elapsed >= 15 && elapsed < 250, "finite wait elapsed %lld ms", elapsed);
    zassert_equal(k_mutex_unlock(&OS_impl_count_sem_table[slot_for_id(id)].lock), 0);

    zassert_equal(k_mutex_lock(&OS_impl_count_sem_table[slot_for_id(id)].lock, K_FOREVER), 0);
    start = k_uptime_get();
    start_worker(0, id, WORK_TIMED_WAIT, 90, PAUSE_NONE);
    workers[0].capture_native_timeout = true;
    k_sleep(K_MSEC(30));
    zassert_equal(k_mutex_unlock(&OS_impl_count_sem_table[slot_for_id(id)].lock), 0);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SEM_TIMEOUT);
    zassert_true(workers[0].native_timeout_ticks > 0);
    zassert_true(workers[0].native_timeout_ticks < k_ms_to_ticks_ceil32(90),
                 "native timeout was reset after admission");
    elapsed = k_uptime_get() - start;
    zassert_true(elapsed >= 70 && elapsed < 250,
                 "remaining-budget wait elapsed %lld ms", elapsed);
    elapsed_to_native_ticks =
        k_ms_to_ticks_ceil64(workers[0].native_entry_ms - start);
    minimum_remaining_ticks = requested_ticks > elapsed_to_native_ticks + 3 ?
                              requested_ticks - elapsed_to_native_ticks - 3 : 0;
    zassert_true(workers[0].native_timeout_ticks >= minimum_remaining_ticks &&
                 workers[0].native_timeout_ticks < requested_ticks,
                 "native timeout did not retain the original deadline");
    TC_PRINT("countsem remaining budget: elapsed %lld ms, native %lld ticks of %u; "
             "entry %lld ms, tick rate %u Hz, tolerance %lld ms\n",
             elapsed, (long long)workers[0].native_timeout_ticks,
             k_ms_to_ticks_ceil32(90), workers[0].native_entry_ms - start,
             CONFIG_SYS_CLOCK_TICKS_PER_SEC,
             tick_tolerance_ms);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);

    create(&id, "uint32-available", 1);
    zassert_equal(OS_CountSemTimedWait(id, UINT32_MAX), OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);

    create(&id, "uint32-finite", 0);
    start_worker(0, id, WORK_TIMED_WAIT, UINT32_MAX, AFTER_VALIDATION);
    workers[0].capture_native_timeout = true;
    release_worker(0);
    zassert_equal(k_sem_take(&workers[0].native_started, K_MSEC(WATCHDOG_MS)), 0,
                  "wide finite timeout did not reach native take");
    zassert_true(workers[0].native_timeout_ticks > 0);
    zassert_true(workers[0].native_timeout_ticks != K_FOREVER.ticks);
    zassert_true(workers[0].native_timeout_ticks >=
                 k_ms_to_ticks_ceil64(UINT32_MAX) - 2 &&
                 workers[0].native_timeout_ticks <=
                 k_ms_to_ticks_ceil64(UINT32_MAX),
                 "wide finite timeout converted to %lld ticks",
                 (long long)workers[0].native_timeout_ticks);
    TC_PRINT("countsem UINT32_MAX finite timeout: %lld ticks\n",
             (long long)workers[0].native_timeout_ticks);
    zassert_equal(OS_CountSemGive(id), OS_SUCCESS);
    join_worker(0);
    zassert_equal(workers[0].result, OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);
}

ZTEST(osal_zephyr_countsem, test_three_waiters_receive_three_real_gives)
{
    osal_id_t id;

    for (int round = 0; round < REPEAT_COUNT; ++round) {
        create(&id, "three-waiters", 0);
        for (int i = 0; i < WORKER_COUNT; ++i) {
            start_worker(i, id, WORK_TAKE, 0, BEFORE_NATIVE_TAKE);
        }
        for (int i = 0; i < WORKER_COUNT; ++i) {
            release_into_native_take(i);
        }
        /* Workers are now in native waits: deletion must preserve the ID. */
        zassert_equal(OS_CountSemDelete(id), OS_SEM_FAILURE);
        for (int i = 0; i < WORKER_COUNT; ++i) {
            zassert_equal(OS_CountSemGive(id), OS_SUCCESS);
            join_worker(i);
            zassert_equal(workers[i].result, OS_SUCCESS);
        }
        zassert_equal(count_of(id), 0);
        zassert_equal(OS_CountSemDelete(id), OS_SUCCESS);
    }
}

ZTEST(osal_zephyr_countsem, test_object_core_neighbours_survive_quiescent_reuse)
{
    osal_id_t ids[3];
    osal_id_t old_id;
    bool expected[OS_MAX_COUNT_SEMAPHORES] = {true, true, true};

    create(&ids[0], "core-neighbor-a", 0);
    create(&ids[1], "core-neighbor-b", 0);
    create(&ids[2], "core-reuse", 0);
    assert_registered_set(expected);
    for (int i = 0; i < REPEAT_COUNT; ++i) {
        old_id = ids[2];
        zassert_equal(OS_CountSemDelete(old_id), OS_SUCCESS);
        expected[slot_for_id(old_id)] = false;
        assert_registered_set(expected);
        create(&ids[2], "core-reuse", 0);
        assert_same_slot_new_id(old_id, ids[2]);
        expected[slot_for_id(ids[2])] = true;
        assert_registered_set(expected);
    }
    for (int i = 0; i < 3; ++i) {
        zassert_equal(OS_CountSemDelete(ids[i]), OS_SUCCESS);
        expected[slot_for_id(ids[i])] = false;
    }
    assert_registered_set(expected);
}

ZTEST(osal_zephyr_countsem, test_abort_after_shared_validation_has_no_reservation)
{
    osal_id_t old_id;
    osal_id_t new_id;
    osal_id_t neighbours[2];

    create(&old_id, "abort-validated", 0);
    create(&neighbours[0], "abort-neighbour-a", 0);
    create(&neighbours[1], "abort-neighbour-b", 0);
    start_worker(0, old_id, WORK_TAKE, 0, AFTER_VALIDATION);
    abort_after_shared_validation(0);
    zassert_equal(OS_CountSemDelete(old_id), OS_SUCCESS);
    create(&new_id, "abort-replacement", 0);
    assert_same_slot_new_id(old_id, new_id);
    zassert_equal(count_of(new_id), 0);
    zassert_equal(OS_CountSemDelete(new_id), OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(neighbours[0]), OS_SUCCESS);
    zassert_equal(OS_CountSemDelete(neighbours[1]), OS_SUCCESS);
}

static void *suite_setup(void)
{
    for (int i = 0; i < WORKER_COUNT; ++i) {
        /* These gates are registered once with object core enabled.  Cases
         * only reset them; reinitializing a linked k_sem is prohibited. */
        k_sem_init(&workers[i].reached, 0, 1);
        k_sem_init(&workers[i].resume, 0, 1);
        k_sem_init(&workers[i].native_started, 0, 1);
        k_sem_init(&workers[i].done, 0, 1);
    }
    zassert_equal(OS_API_Init(), OS_SUCCESS);
    return NULL;
}

ZTEST_SUITE(osal_zephyr_countsem, NULL, suite_setup, NULL, NULL, NULL);
