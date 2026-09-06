/*
 * Copyright (c) 2026 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

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
    OS_impl_binsem_internal_record_t *zero_impl    = &OS_impl_bin_sem_table[zero_token.obj_idx];
    OS_impl_binsem_internal_record_t *one_impl     = &OS_impl_bin_sem_table[one_token.obj_idx];
    OS_impl_binsem_internal_record_t *nonzero_impl = &OS_impl_bin_sem_table[nonzero_token.obj_idx];
    OS_bin_sem_prop_t                 prop;
    int32                              status;

    harness_start(&h, "binsem");

    status = OS_BinSemCreate_Impl(&zero_token, 0, 0);
    harness_check(&h, status == OS_SUCCESS, "create-zero");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    harness_check(&h, zero_impl->current_value == 0, "zero-value");

    status = OS_BinSemCreate_Impl(&one_token, 1, 0);
    harness_check(&h, status == OS_SUCCESS, "create-one");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    harness_check(&h, one_impl->current_value == 1, "one-value");

    status = OS_BinSemCreate_Impl(&nonzero_token, UINT32_MAX, 0);
    harness_check(&h, status == OS_SUCCESS, "create-nonzero");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    harness_check(&h, nonzero_impl->current_value == 1, "nonzero-normalized");

    /* Take() on a semaphore that is already available must not block. */
    status = OS_BinSemTake_Impl(&one_token);
    harness_check(&h, status == OS_SUCCESS, "take-one");
    harness_check(&h, one_impl->current_value == 0, "one-taken");
    harness_check(&h, zero_impl->current_value == 0, "zero-independent");
    harness_check(&h, nonzero_impl->current_value == 1, "nonzero-independent");

    status = OS_BinSemGive_Impl(&zero_token);
    harness_check(&h, status == OS_SUCCESS, "give-zero");
    harness_check(&h, zero_impl->current_value == 1, "zero-given");

    /* Binary semaphores saturate at one; a second Give() is not an error. */
    status = OS_BinSemGive_Impl(&zero_token);
    harness_check(&h, status == OS_SUCCESS, "give-saturated");
    harness_check(&h, zero_impl->current_value == 1, "zero-saturated");

    status = OS_BinSemTake_Impl(&zero_token);
    harness_check(&h, status == OS_SUCCESS, "take-zero");
    harness_check(&h, zero_impl->current_value == 0, "zero-retaken");
    harness_check(&h, one_impl->current_value == 0, "one-still-taken");
    harness_check(&h, nonzero_impl->current_value == 1, "nonzero-still-set");

    /* With no Give() pending, a timed wait on an unavailable semaphore
     * must return OS_SEM_TIMEOUT without touching its value. */
    status = OS_BinSemTimedWait_Impl(&zero_token, 20);
    harness_check(&h, status == OS_SEM_TIMEOUT, "timedwait-times-out");
    harness_check(&h, zero_impl->current_value == 0, "zero-unchanged-after-timeout");

    /* Flush() must wake pending waiters without fabricating availability,
     * and a later Take() must not treat a stale flush generation as a
     * release. */
    status = OS_BinSemFlush_Impl(&zero_token);
    harness_check(&h, status == OS_SUCCESS, "flush-zero");
    harness_check(&h, zero_impl->flush_request == 1, "flush-recorded");
    harness_check(&h, zero_impl->current_value == 0, "flush-does-not-give");

    status = OS_BinSemTimedWait_Impl(&zero_token, 20);
    harness_check(&h, status == OS_SEM_TIMEOUT, "timedwait-after-flush-times-out");

    status = OS_BinSemGive_Impl(&zero_token);
    harness_check(&h, status == OS_SUCCESS, "give-before-take");
    status = OS_BinSemTake_Impl(&zero_token);
    harness_check(&h, status == OS_SUCCESS, "give-then-take");
    harness_check(&h, zero_impl->current_value == 0, "give-then-take-consumed");

    status = OS_BinSemGetInfo_Impl(&nonzero_token, &prop);
    harness_check(&h, status == OS_SUCCESS && prop.value == 1, "getinfo-nonzero");

    /* A slot must remain usable across delete/recreate. */
    status = OS_BinSemDelete_Impl(&nonzero_token);
    harness_check(&h, status == OS_SUCCESS, "delete-nonzero");

    status = OS_BinSemCreate_Impl(&nonzero_token, 0, 0);
    harness_check(&h, status == OS_SUCCESS, "recreate-after-delete");
    harness_check(&h, nonzero_impl->current_value == 0, "recreate-value");

    return harness_finish(&h);
}
