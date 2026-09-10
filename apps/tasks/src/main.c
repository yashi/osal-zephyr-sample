/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>

#include "harness.h"
#include "osapi.h"

K_SEM_DEFINE(ready, 0, 1);
K_SEM_DEFINE(release_worker, 0, 1);
static osal_id_t observed_id;
static int32 delay_status;
static bool custom_data_preserved;
static bool reached_after_wait;
static int custom_data;

static void worker(void)
{
    k_thread_custom_data_set(&custom_data);
    observed_id = OS_TaskGetId();
    custom_data_preserved = k_thread_custom_data_get() == &custom_data;
    delay_status = OS_TaskDelay(1);
    k_sem_give(&ready);
    k_sem_take(&release_worker, K_FOREVER);
    reached_after_wait = true;
}

static bool wait_for_exit(const char *name)
{
    osal_id_t id;
    int64_t end = k_uptime_get() + 5000;

    do {
        if (OS_TaskGetIdByName(&id, name) == OS_ERR_NAME_NOT_FOUND) {
            return true;
        }
        k_msleep(1);
    } while (k_uptime_get() < end);
    return false;
}

int main(void)
{
    struct harness h;
    osal_id_t id;
    osal_id_t found;
    OS_task_prop_t prop;
    int32 status;

    harness_start(&h, "tasks");
    status = OS_API_Init();
    harness_check(&h, status == OS_SUCCESS, "initialize");
    if (status != OS_SUCCESS) {
        return harness_finish(&h);
    }
    harness_check(&h, !OS_ObjectIdDefined(OS_TaskGetId()), "external-identity");
    status = OS_TaskCreate(&id, "graceful", worker, NULL, 2048, 128, 0);
    harness_check(&h, status == OS_SUCCESS, "create-graceful");
    if (status != OS_SUCCESS) {
        return harness_finish(&h);
    }
    bool started = k_sem_take(&ready, K_SECONDS(5)) == 0;
    harness_check(&h, started, "graceful-started");
    if (!started) {
        return harness_finish(&h);
    }
    harness_check(&h, OS_ObjectIdEqual(id, observed_id), "managed-identity");
    harness_check(&h, custom_data_preserved, "application-custom-data");
    harness_check(&h, delay_status == OS_SUCCESS, "task-delay");
    status = OS_TaskGetInfo(id, &prop);
    harness_check(&h, status == OS_SUCCESS && strcmp(prop.name, "graceful") == 0 &&
                  prop.stack_size == 2048 && prop.priority == 128, "task-info");
    status = OS_TaskGetIdByName(&found, "graceful");
    harness_check(&h, status == OS_SUCCESS && OS_ObjectIdEqual(found, id), "name-lookup");
    harness_check(&h, OS_TaskSetPriority(id, 200) == OS_SUCCESS, "set-priority");
    status = OS_TaskGetInfo(id, &prop);
    harness_check(&h, status == OS_SUCCESS && prop.priority == 200, "updated-priority");
    k_sem_give(&release_worker);
    bool exited = wait_for_exit("graceful");
    harness_check(&h, exited && reached_after_wait, "graceful-return");
    if (!exited) {
        return harness_finish(&h);
    }
    harness_check(&h, OS_TaskGetInfo(id, &prop) == OS_ERR_INVALID_ID, "retired-id");

    reached_after_wait = false;
    status = OS_TaskCreate(&id, "delete", worker, NULL, 2048, 128, 0);
    harness_check(&h, status == OS_SUCCESS, "create-delete");
    if (status != OS_SUCCESS) {
        return harness_finish(&h);
    }
    started = k_sem_take(&ready, K_SECONDS(5)) == 0;
    harness_check(&h, started, "delete-started");
    if (!started) {
        return harness_finish(&h);
    }
    /* This worker owns no resource; its test semaphore wait needs no cleanup. */
    status = OS_TaskDelete(id);
    harness_check(&h, status == OS_SUCCESS, "delete-resource-free-task");
    k_sem_give(&release_worker);
    k_msleep(1);
    harness_check(&h, !reached_after_wait, "deleted-task-stopped");
    harness_check(&h, OS_TaskGetInfo(id, &prop) == OS_ERR_INVALID_ID, "deleted-id");
    return harness_finish(&h);
}
