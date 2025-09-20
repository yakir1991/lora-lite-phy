cmake_minimum_required(VERSION 3.16)
project(lora_gr_pipeline LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_library(LIQUID_LIB liquid REQUIRED)

# Find pybind11
find_package(pybind11 REQUIRED)

add_library(lora_gr
    src/workspace.cpp
    src/rx/gr/utils.cpp
    src/rx/gr/primitives.cpp
    src/rx/gr/header_decode.cpp
    src/rx/gr_pipeline.cpp
)

target_include_directories(lora_gr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lora_gr PUBLIC ${LIQUID_LIB})

add_executable(test_gr_pipeline test_gr_pipeline.cpp)

target_link_libraries(test_gr_pipeline PRIVATE lora_gr)

# Python bindings
pybind11_add_module(lora_pipeline python_bindings.cpp)
target_link_libraries(lora_pipeline PRIVATE lora_gr pybind11::module)
