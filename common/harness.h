/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OSAL_HARNESS_H
#define OSAL_HARNESS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

struct harness {
	const char *name;
	unsigned int checks;
	unsigned int failures;
};

static inline void harness_start(struct harness *h, const char *name)
{
	h->name = name;
	h->checks = 0;
	h->failures = 0;
	printf("HARNESS START %s\n", name);
}

static inline void harness_check(struct harness *h, bool condition, const char *label)
{
	h->checks++;
	if (!condition) {
		h->failures++;
	}
	printf("HARNESS CHECK %s %s\n", label, condition ? "PASS" : "FAIL");
}

static inline int harness_finish(struct harness *h)
{
	int status = h->checks == 0 || h->failures != 0;

	printf("HARNESS RESULT %s %s checks=%u failures=%u\n", h->name,
	       status ? "FAIL" : "PASS", h->checks, h->failures);
	fflush(stdout);
#if defined(CONFIG_ARCH_POSIX)
	/* Returning from Zephyr's main thread does not convey a host exit status. */
	exit(status);
#endif
	return status;
}

#endif /* OSAL_HARNESS_H */
