# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Three slots let the test retain two registered neighbours while exercising
# logical deletion and reuse of the remaining provider object.
set(OSAL_CONFIG_MAX_COUNT_SEMAPHORES 3 CACHE STRING
    "Maximum Number of Counting Semaphores to support")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_zephyr_countsem)

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
# These wrappers observe only actual successful shared validation and actual
# native boundaries.  They never supply a provider result or mutate state.
target_link_options(app PUBLIC
  LINKER:--wrap=OS_ObjectIdGetById
  LINKER:--wrap=z_impl_k_mutex_lock
  LINKER:--wrap=z_impl_k_sem_take
  LINKER:--wrap=z_impl_k_sem_give
)
