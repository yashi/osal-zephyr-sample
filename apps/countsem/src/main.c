/*
 * Copyright (c) 2026 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <stdint.h>

#include "harness.h"
#include "os-impl-countsem.h"
#include "os-shared-countsem.h"
#include "os-shared-idmap.h"

#if OS_MAX_COUNT_SEMAPHORES < 5
#error "The countsem harness requires at least five counting semaphore slots"
#endif

static OS_object_token_t countsem_token(unsigned int index)
{
    OS_object_token_t token = {
        .lock_mode = OS_LOCK_MODE_NONE,
        .obj_type  = OS_OBJECT_TYPE_OS_COUNTSEM,
        .obj_idx   = index,
    };

    return token;
}

static void check_value(struct harness *h, const OS_object_token_t *token,
                        int32 expected, const char *label)
{
    OS_count_sem_prop_t prop;
    int32               status;

    status = OS_CountSemGetInfo_Impl(token, &prop);
    harness_check(h, status == OS_SUCCESS && prop.value == expected, label);
}

int main(void)
{
    struct harness h;
    OS_object_token_t zero = countsem_token(0);
    OS_object_token_t one = countsem_token(1);
    OS_object_token_t three = countsem_token(2);
    OS_object_token_t maximum = countsem_token(3);
    OS_object_token_t invalid = countsem_token(4);
    int32             status;

    harness_start(&h, "countsem");

    status = OS_CountSemCreate_Impl(&zero, 0, 0);
    harness_check(&h, status == OS_SUCCESS, "create-zero");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    status = OS_CountSemCreate_Impl(&one, 1, 0);
    harness_check(&h, status == OS_SUCCESS, "create-one");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    status = OS_CountSemCreate_Impl(&three, 3, 0);
    harness_check(&h, status == OS_SUCCESS, "create-three");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    check_value(&h, &zero, 0, "zero-info");
    check_value(&h, &one, 1, "one-info");
    check_value(&h, &three, 3, "three-info");

    status = OS_CountSemTimedWait_Impl(&zero, 0);
    harness_check(&h, status == OS_SEM_TIMEOUT, "zero-poll-timeout");
    status = OS_CountSemTimedWait_Impl(&zero, 20);
    harness_check(&h, status == OS_SEM_TIMEOUT, "zero-finite-timeout");

    status = OS_CountSemGive_Impl(&zero);
    harness_check(&h, status == OS_SUCCESS, "zero-give");
    check_value(&h, &zero, 1, "zero-give-count");
    status = OS_CountSemTake_Impl(&zero);
    harness_check(&h, status == OS_SUCCESS, "zero-take");
    check_value(&h, &zero, 0, "zero-take-count");

    status = OS_CountSemTake_Impl(&one);
    harness_check(&h, status == OS_SUCCESS, "one-take");
    status = OS_CountSemTimedWait_Impl(&one, 0);
    harness_check(&h, status == OS_SEM_TIMEOUT, "one-empty-poll");
    check_value(&h, &three, 3, "three-independent-value");

    status = OS_CountSemGive_Impl(&three);
    harness_check(&h, status == OS_SUCCESS, "three-give");
    check_value(&h, &three, 4, "three-counts-up");
    status = OS_CountSemTake_Impl(&three);
    harness_check(&h, status == OS_SUCCESS, "three-take");
    check_value(&h, &three, 3, "three-counts-down");

    status = OS_CountSemCreate_Impl(&maximum, INT32_MAX, 0);
    harness_check(&h, status == OS_SUCCESS, "create-maximum");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    check_value(&h, &maximum, INT32_MAX, "maximum-info");
    status = OS_CountSemGive_Impl(&maximum);
    harness_check(&h, status == OS_SEM_FAILURE, "maximum-give-overflow");
    check_value(&h, &maximum, INT32_MAX, "maximum-overflow-preserved");
    status = OS_CountSemTake_Impl(&maximum);
    harness_check(&h, status == OS_SUCCESS, "maximum-take");
    check_value(&h, &maximum, INT32_MAX - 1, "maximum-after-take");
    status = OS_CountSemGive_Impl(&maximum);
    harness_check(&h, status == OS_SUCCESS, "maximum-recovery-give");
    check_value(&h, &maximum, INT32_MAX, "maximum-recovered");

    status = OS_CountSemCreate_Impl(&invalid, UINT32_MAX, 0);
    harness_check(&h, status == OS_INVALID_SEM_VALUE, "reject-unsigned-over-cap");
    status = OS_CountSemCreate_Impl(&invalid, (uint32)INT32_MAX + 1U, 0);
    harness_check(&h, status == OS_INVALID_SEM_VALUE, "reject-signed-over-cap");

    status = OS_CountSemDelete_Impl(&zero);
    harness_check(&h, status == OS_SUCCESS, "delete-zero");
    status = OS_CountSemCreate_Impl(&zero, 2, 0);
    harness_check(&h, status == OS_SUCCESS, "recreate-zero");
    if (status != OS_SUCCESS)
    {
        return harness_finish(&h);
    }
    check_value(&h, &zero, 2, "recreated-value");
    status = OS_CountSemTimedWait_Impl(&zero, 0);
    harness_check(&h, status == OS_SUCCESS, "recreated-poll-success");
    check_value(&h, &zero, 1, "recreated-poll-count");

    status = OS_CountSemDelete_Impl(&zero);
    harness_check(&h, status == OS_SUCCESS, "cleanup-zero");
    status = OS_CountSemDelete_Impl(&one);
    harness_check(&h, status == OS_SUCCESS, "cleanup-one");
    status = OS_CountSemDelete_Impl(&three);
    harness_check(&h, status == OS_SUCCESS, "cleanup-three");
    status = OS_CountSemDelete_Impl(&maximum);
    harness_check(&h, status == OS_SUCCESS, "cleanup-maximum");

    return harness_finish(&h);
}
