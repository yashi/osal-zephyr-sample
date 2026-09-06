/*
 * Copyright (c) 2026 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sem.h>

#include "harness.h"
#include "os-impl-binsem.h"
#include "os-shared-binsem.h"
#include "os-shared-idmap.h"

#if OS_MAX_BIN_SEMAPHORES < 3
#error "The binsem harness requires at least three binary semaphore slots"
#endif

int main(void)
{
    struct harness h;
    OS_object_token_t zero_token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_BINSEM,
        .obj_idx   = 0,
    };
    OS_object_token_t one_token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_BINSEM,
        .obj_idx   = 1,
    };
    OS_object_token_t nonzero_token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_BINSEM,
        .obj_idx   = 2,
    };
    struct sys_sem *zero_sem    = &OS_impl_bin_sem_table[zero_token.obj_idx].sem;
    struct sys_sem *one_sem     = &OS_impl_bin_sem_table[one_token.obj_idx].sem;
    struct sys_sem *nonzero_sem = &OS_impl_bin_sem_table[nonzero_token.obj_idx].sem;
    int32           status;
    int             native_status;

    harness_start(&h, "binsem");

    status = OS_BinSemCreate_Impl(&zero_token, 0, 0);
    harness_check(&h, status == OS_SUCCESS, "create-zero");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    harness_check(&h, sys_sem_count_get(zero_sem) == 0, "zero-count");

    status = OS_BinSemCreate_Impl(&one_token, 1, 0);
    harness_check(&h, status == OS_SUCCESS, "create-one");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    harness_check(&h, sys_sem_count_get(one_sem) == 1, "one-count");

    status = OS_BinSemCreate_Impl(&nonzero_token, UINT32_MAX, 0);
    harness_check(&h, status == OS_SUCCESS, "create-nonzero");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    harness_check(&h, sys_sem_count_get(nonzero_sem) == 1, "nonzero-normalized");

    native_status = sys_sem_take(one_sem, K_NO_WAIT);
    harness_check(&h, native_status == 0, "take-one");
    harness_check(&h, sys_sem_count_get(one_sem) == 0, "one-taken");
    harness_check(&h, sys_sem_count_get(zero_sem) == 0, "zero-independent");
    harness_check(&h, sys_sem_count_get(nonzero_sem) == 1, "nonzero-independent");

    native_status = sys_sem_give(zero_sem);
    harness_check(&h, native_status == 0, "give-zero");
    harness_check(&h, sys_sem_count_get(zero_sem) == 1, "zero-given");

    native_status = sys_sem_give(zero_sem);
    harness_check(&h, native_status == 0 || native_status == -EAGAIN, "give-saturated");
    harness_check(&h, sys_sem_count_get(zero_sem) == 1, "zero-saturated");

    native_status = sys_sem_take(zero_sem, K_NO_WAIT);
    harness_check(&h, native_status == 0, "take-zero");
    harness_check(&h, sys_sem_count_get(zero_sem) == 0, "zero-retaken");
    harness_check(&h, sys_sem_count_get(one_sem) == 0, "one-still-taken");
    harness_check(&h, sys_sem_count_get(nonzero_sem) == 1, "nonzero-still-set");

    return harness_finish(&h);
}
