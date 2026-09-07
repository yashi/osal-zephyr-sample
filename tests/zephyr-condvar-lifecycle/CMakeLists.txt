# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# One slot makes a deleted object's native storage reuse observable.
set(OSAL_CONFIG_MAX_CONDVARS 1 CACHE STRING
    "Maximum Number of Condition Variables to support")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_zephyr_condvar_lifecycle)

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
# The wrappers only observe real shared and Zephyr kernel boundaries.
target_link_options(app PUBLIC
  LINKER:--wrap=OS_ObjectIdGetById
  LINKER:--wrap=z_impl_k_mutex_lock
  LINKER:--wrap=z_impl_k_condvar_wait
  LINKER:--wrap=z_impl_k_condvar_signal
  LINKER:--wrap=z_impl_k_condvar_broadcast
)
