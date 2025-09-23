cmake_minimum_required(VERSION 3.16)
project(lora_lite_standalone_rx CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(lora_rx_core
    src/dsp.cpp
    src/state_machine.cpp
)

target_include_directories(lora_rx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(lora_rx src/main.cpp)

target_link_libraries(lora_rx PRIVATE lora_rx_core)

# Enable high warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lora_rx_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(lora_rx PRIVATE -Wall -Wextra -Wpedantic)
endif()
