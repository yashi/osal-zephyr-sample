# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
set(OSAL_CONFIG_MAX_TASKS 4 CACHE STRING "Maximum Number of Tasks to support")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_tasks_harness)
target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../../common)
target_link_libraries(app PRIVATE osal)
