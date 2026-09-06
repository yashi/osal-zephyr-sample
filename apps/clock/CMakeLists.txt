# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_clock_harness)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../../common)
target_link_libraries(app PRIVATE osal)
