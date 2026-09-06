# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_binsem_harness)

get_target_property(SELECTED_OSAL_SOURCE_DIR osal SOURCE_DIR)
if(NOT SELECTED_OSAL_SOURCE_DIR)
  message(FATAL_ERROR "The selected osal target has no source directory")
endif()

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ../../common
  ${SELECTED_OSAL_SOURCE_DIR}/src/os/shared/inc
  ${SELECTED_OSAL_SOURCE_DIR}/src/os/zephyr/inc
)
target_link_libraries(app PRIVATE osal)
