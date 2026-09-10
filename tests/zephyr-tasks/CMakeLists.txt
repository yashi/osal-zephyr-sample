# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(OSAL_CONFIG_MAX_TASKS 4 CACHE STRING
    "Maximum Number of Tasks to support")
set(OSAL_CONFIG_MAX_COUNT_SEMAPHORES 1 CACHE STRING
    "Maximum Number of Counting Semaphores to support")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_zephyr_tasks)

get_target_property(SELECTED_OSAL_SOURCE_DIR osal SOURCE_DIR)
if(NOT SELECTED_OSAL_SOURCE_DIR)
  message(FATAL_ERROR "The selected osal target has no source directory")
endif()

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ${SELECTED_OSAL_SOURCE_DIR}/src/os/shared/inc
  ${SELECTED_OSAL_SOURCE_DIR}/src/os/zephyr/inc
)
target_link_libraries(app PRIVATE osal)
target_link_options(app PUBLIC
  LINKER:--wrap=OS_ObjectIdGetById
  LINKER:--wrap=z_impl_k_sem_take
  LINKER:--wrap=z_impl_k_thread_stack_alloc
  LINKER:--wrap=z_impl_k_thread_stack_free
)
