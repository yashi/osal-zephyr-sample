/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include "osapi.h"
#include "os-shared-common.h"
#include "os-shared-console.h"
#include "os-shared-idmap.h"
#include "os-shared-printf.h"
#include "bsp-impl.h"
#include "harness.h"

int main(void)
{
	struct harness h;
	osal_id_t console_id;
	OS_object_token_t token;
	int32 status;

	harness_start(&h, "init");
	status = OS_API_Init();
	harness_check(&h, status == OS_SUCCESS, "api_init");
	if (status != OS_SUCCESS) {
		return harness_finish(&h);
	}
	harness_check(&h, OS_SharedGlobalVars.GlobalState == OS_INIT_MAGIC_NUMBER,
		      "initialized_state");
	harness_check(&h, OS_SharedGlobalVars.TicksPerSecond == CONFIG_SYS_CLOCK_TICKS_PER_SEC &&
		      OS_SharedGlobalVars.MicroSecPerTick == 1000000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
		      "tick_globals");
	harness_check(&h, !OS_ObjectIdDefined(OS_TaskGetId()), "external_thread_identity");
	console_id = OS_SharedGlobalVars.PrintfConsoleId;
	harness_check(&h, OS_IdentifyObject(console_id) == OS_OBJECT_TYPE_OS_CONSOLE,
		      "console_id");
	OS_printf("OS_printf ready\n");
	status = OS_ObjectIdGetById(OS_LOCK_MODE_GLOBAL, OS_OBJECT_TYPE_OS_CONSOLE,
				   console_id, &token);
	if (status == OS_SUCCESS) {
		OS_console_internal_record_t *console = OS_OBJECT_TABLE_GET(OS_console_table, token);

		harness_check(&h, !console->IsAsync && console->ReadPos == console->WritePos,
			      "console_drained");
		OS_ObjectIdRelease(&token);
	} else {
		harness_check(&h, false, "console_drained");
	}
	harness_check(&h, OS_API_Init() == OS_SUCCESS &&
		      OS_ObjectIdEqual(console_id, OS_SharedGlobalVars.PrintfConsoleId),
		      "redundant_init");
	OS_BSP_SetExitCode(OS_ERROR);
	harness_check(&h, OS_BSP_Global.AppStatus == OS_ERROR, "bsp_failure_status");
	OS_BSP_SetExitCode(OS_SUCCESS);
	OS_ApplicationShutdown(true);
	OS_IdleLoop();
	harness_check(&h, OS_SharedGlobalVars.GlobalState == OS_SHUTDOWN_MAGIC_NUMBER,
		      "shutdown_before_idle");
	return harness_finish(&h);
}
