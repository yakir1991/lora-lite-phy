cmake_minimum_required(VERSION 3.16)
project(lora_host_sim LANGUAGES C CXX)

include(CTest)

add_subdirectory(host_sim)

# --- Examples (opt-in with -DBUILD_EXAMPLES=ON) ---
option(BUILD_EXAMPLES "Build example programs" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
