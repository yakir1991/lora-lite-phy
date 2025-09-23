cmake_minimum_required(VERSION 3.16)
project(lora_gr_pipeline LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED LIQUID_LIB OR LIQUID_LIB STREQUAL "")
    unset(LIQUID_LIB CACHE)
    find_library(
        LIQUID_LIB
        NAMES liquid
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/local_lib
            ${CMAKE_CURRENT_SOURCE_DIR}/external/liquid-dsp/build
            ${CMAKE_CURRENT_SOURCE_DIR}/external/liquid-dsp
            /usr/lib
            /usr/lib/x86_64-linux-gnu
        PATH_SUFFIXES "" lib
        NO_DEFAULT_PATH
    )
endif()

if(NOT DEFINED LIQUID_LIB OR LIQUID_LIB STREQUAL "")
    unset(LIQUID_LIB CACHE)
    find_library(LIQUID_LIB NAMES liquid)
endif()

if(NOT DEFINED LIQUID_LIB OR LIQUID_LIB STREQUAL "")
    set(LIQUID_SUBMODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/liquid-dsp)
    if(EXISTS ${LIQUID_SUBMODULE_DIR}/CMakeLists.txt)
        message(STATUS "liquid-dsp not found on host; building from ${LIQUID_SUBMODULE_DIR}")
        set(BUILD_EXAMPLES OFF CACHE BOOL "Disable liquid-dsp examples" FORCE)
        set(BUILD_AUTOTESTS OFF CACHE BOOL "Disable liquid-dsp autotests" FORCE)
        set(BUILD_BENCHMARKS OFF CACHE BOOL "Disable liquid-dsp benchmarks" FORCE)
        set(BUILD_SANDBOX OFF CACHE BOOL "Disable liquid-dsp sandbox" FORCE)
        set(BUILD_DOC OFF CACHE BOOL "Disable liquid-dsp docs" FORCE)
        add_subdirectory(${LIQUID_SUBMODULE_DIR} external/liquid-dsp)
        set(LIQUID_LIB liquid)
        set(LIQUID_INCLUDE_DIR ${LIQUID_SUBMODULE_DIR}/include)
    else()
        message(FATAL_ERROR "Could not find liquid-dsp. Install it or initialise external/liquid-dsp.")
    endif()
endif()

if(NOT LIQUID_INCLUDE_DIR)
    find_path(
        LIQUID_INCLUDE_DIR
        NAMES liquid/liquid.h
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/external/liquid-dsp/include
            ${CMAKE_CURRENT_SOURCE_DIR}/local_lib/include
    )
endif()

if(LIQUID_INCLUDE_DIR STREQUAL "LIQUID_INCLUDE_DIR-NOTFOUND")
    set(LIQUID_INCLUDE_DIR "")
endif()

# pybind11 removed - using scheduler only

add_library(lora_gr
    src/workspace.cpp
    src/rx/gr/utils.cpp
    src/rx/gr/primitives.cpp
    src/rx/gr/header_decode.cpp
    src/rx/scheduler/scheduler.cpp
)

target_include_directories(lora_gr PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIQUID_INCLUDE_DIR}
)
target_link_libraries(lora_gr PUBLIC ${LIQUID_LIB})

add_executable(test_gr_pipeline tests/test_gr_pipeline.cpp)

target_link_libraries(test_gr_pipeline PRIVATE lora_gr)

# Python bindings removed - using scheduler only

# Scheduler unit tests
add_executable(test_scheduler tests/test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE lora_gr)

# Scheduler example
add_executable(scheduler_example examples/scheduler_example.cpp)
target_link_libraries(scheduler_example PRIVATE lora_gr)
