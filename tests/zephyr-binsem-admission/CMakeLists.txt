# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# This fixture limits the provider to one slot so reuse is observable.
set(OSAL_CONFIG_MAX_BIN_SEMAPHORES 1 CACHE STRING
    "Maximum Number of Binary Semaphores to support")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_zephyr_binsem_admission)

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
# app is an archive: propagate wrappers to the final executable link.
target_link_options(app PUBLIC
  LINKER:--wrap=OS_ObjectIdGetById
  LINKER:--wrap=z_impl_k_mutex_lock
  LINKER:--wrap=z_impl_k_condvar_wait
)
