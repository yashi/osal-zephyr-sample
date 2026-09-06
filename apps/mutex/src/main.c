/*
 * Copyright (c) 2026 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "harness.h"
#include "os-shared-idmap.h"
#include "os-shared-mutex.h"

#if OS_MAX_MUTEXES < 3
#error "The mutex harness requires at least three mutex slots"
#endif

int main(void)
{
    struct harness h;
    OS_object_token_t a_token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_MUTEX,
        .obj_idx   = 0,
    };
    OS_object_token_t b_token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_MUTEX,
        .obj_idx   = 1,
    };
    OS_object_token_t c_token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_MUTEX,
        .obj_idx   = 2,
    };
    OS_mut_sem_prop_t prop;
    int32              status;

    harness_start(&h, "mutex");

    status = OS_MutSemCreate_Impl(&a_token, 0);
    harness_check(&h, status == OS_SUCCESS, "create-a");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }

    status = OS_MutSemCreate_Impl(&b_token, 0);
    harness_check(&h, status == OS_SUCCESS, "create-b");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }

    status = OS_MutSemCreate_Impl(&c_token, 0);
    harness_check(&h, status == OS_SUCCESS, "create-c");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }

    /* A mutex that was never taken must reject Give(). */
    status = OS_MutSemGive_Impl(&b_token);
    harness_check(&h, status != OS_SUCCESS, "give-without-take-fails");

    status = OS_MutSemTake_Impl(&a_token);
    harness_check(&h, status == OS_SUCCESS, "take-a");

    /* Holding "a" must not block or otherwise affect "b". */
    status = OS_MutSemTake_Impl(&b_token);
    harness_check(&h, status == OS_SUCCESS, "take-b-while-a-held");

    /* Zephyr mutexes are recursive: the same thread can re-take "a"
     * without blocking. */
    status = OS_MutSemTake_Impl(&a_token);
    harness_check(&h, status == OS_SUCCESS, "recursive-take-a");

    status = OS_MutSemGive_Impl(&b_token);
    harness_check(&h, status == OS_SUCCESS, "give-b");

    /* "b" was taken once, so a second Give() must fail. */
    status = OS_MutSemGive_Impl(&b_token);
    harness_check(&h, status != OS_SUCCESS, "give-b-again-fails");

    status = OS_MutSemGive_Impl(&a_token);
    harness_check(&h, status == OS_SUCCESS, "give-a-first");

    status = OS_MutSemGive_Impl(&a_token);
    harness_check(&h, status == OS_SUCCESS, "give-a-second");

    /* "a" was taken exactly twice, so a third Give() must fail. */
    status = OS_MutSemGive_Impl(&a_token);
    harness_check(&h, status != OS_SUCCESS, "give-a-extra-fails");

    status = OS_MutSemGetInfo_Impl(&c_token, &prop);
    harness_check(&h, status == OS_SUCCESS, "getinfo-c");

    /* A slot must remain usable across delete/recreate. */
    status = OS_MutSemDelete_Impl(&c_token);
    harness_check(&h, status == OS_SUCCESS, "delete-c");

    status = OS_MutSemCreate_Impl(&c_token, 0);
    harness_check(&h, status == OS_SUCCESS, "recreate-c");

    status = OS_MutSemTake_Impl(&c_token);
    harness_check(&h, status == OS_SUCCESS, "take-recreated-c");

    status = OS_MutSemGive_Impl(&c_token);
    harness_check(&h, status == OS_SUCCESS, "give-recreated-c");

    return harness_finish(&h);
}
