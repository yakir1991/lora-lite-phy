cmake_minimum_required(VERSION 3.16)
project(lora_host_sim VERSION 0.2.0 LANGUAGES C CXX)

# --- Code coverage instrumentation (opt-in: -DENABLE_COVERAGE=ON) ---
# This has to run before add_subdirectory(host_sim): add_compile_options and
# add_link_options only affect targets created afterwards, so if it came later
# the flags would miss every target and no .gcda files would be produced.
option(ENABLE_COVERAGE "Build with gcov code-coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# LTO and gcov do not mix — inlined bodies lose their line mapping — so only
# enable interprocedural optimisation when not building for coverage.
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
if(IPO_SUPPORTED AND NOT ENABLE_COVERAGE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

include(CTest)

add_subdirectory(host_sim)

# --- Examples (opt-in with -DBUILD_EXAMPLES=ON) ---
option(BUILD_EXAMPLES "Build example programs" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Doxygen (opt-in: cmake --build . --target docs) ---
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()

# --- Code coverage report target (cmake --build . --target coverage) ---
# The instrumentation flags are set at the top of this file; here we only wire
# up the lcov/genhtml report generation. --ignore-errors is broadened so a
# gcov/lcov version skew on CI degrades gracefully instead of failing the job.
if(ENABLE_COVERAGE)
    find_program(LCOV lcov)
    find_program(GENHTML genhtml)
    if(LCOV AND GENHTML)
        add_custom_target(coverage
            COMMAND ${LCOV} --capture --directory . --output-file coverage.info
                    --ignore-errors mismatch,gcov,source,empty
            COMMAND ${LCOV} --remove coverage.info '/usr/*' '*/third_party/*'
                    --output-file coverage.info --ignore-errors unused,empty
            COMMAND ${GENHTML} coverage.info --output-directory coverage_html
                    --ignore-errors source,empty
            COMMAND ${CMAKE_COMMAND} -E echo "Coverage report: ${CMAKE_BINARY_DIR}/coverage_html/index.html"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating code coverage report"
            VERBATIM
        )
    endif()
endif()
