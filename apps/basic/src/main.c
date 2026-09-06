/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "osapi-bsp.h"
#include "osapi-version.h"
#include "harness.h"

int main(void)
{
	struct harness h;
	uint8 version[4];
	const char *version_string;

	harness_start(&h, "basic");

	/* These accessors exercise the linked OSAL and BSP libraries before init. */
	version_string = OS_GetVersionString();
	harness_check(&h, version_string != NULL &&
		      strcmp(version_string, OS_VERSION) == 0, "version-string");
	OS_GetVersionNumber(version);
	harness_check(&h, version[0] == OS_MAJOR_VERSION &&
		      version[1] == OS_MINOR_VERSION &&
		      version[2] == OS_REVISION &&
		      version[3] == OS_MISSION_REV, "version-number");
	harness_check(&h, OS_BSP_GetArgC() == 0, "bsp-argc");
	harness_check(&h, OS_BSP_GetArgV() == NULL, "bsp-argv");

	return harness_finish(&h);
}
