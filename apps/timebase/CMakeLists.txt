# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(OSAL_CONFIG_MAX_TIMEBASES 2 CACHE STRING
    "Maximum Number of Timebases to support")
set(OSAL_CONFIG_MAX_TIMERS 3 CACHE STRING
    "Maximum Number of Timers to support")
set(OSAL_CONFIG_MAX_TASKS 2 CACHE STRING
    "Maximum Number of Tasks to support")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_timebase_harness)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../../common)
target_link_libraries(app PRIVATE osal)
