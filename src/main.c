/*
 * Copyright (c) 2025 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "osapi.h"

int main(void)
{
	printf("Hello cFS OSAL!\n");

	OS_Application_Startup();

	return 0;
}
