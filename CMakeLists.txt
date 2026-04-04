cmake_minimum_required(VERSION 3.16)
project(lora_host_sim VERSION 0.2.0 LANGUAGES C CXX)

include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
if(IPO_SUPPORTED)
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

# --- Code coverage (opt-in: -DENABLE_COVERAGE=ON) ---
option(ENABLE_COVERAGE "Build with gcov code-coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
    # Convenience target: cmake --build . --target coverage
    find_program(LCOV lcov)
    find_program(GENHTML genhtml)
    if(LCOV AND GENHTML)
        add_custom_target(coverage
            COMMAND ${LCOV} --capture --directory . --output-file coverage.info
                    --ignore-errors mismatch
            COMMAND ${LCOV} --remove coverage.info '/usr/*' '*/third_party/*'
                    --output-file coverage.info --ignore-errors unused
            COMMAND ${GENHTML} coverage.info --output-directory coverage_html
            COMMAND ${CMAKE_COMMAND} -E echo "Coverage report: ${CMAKE_BINARY_DIR}/coverage_html/index.html"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating code coverage report"
            VERBATIM
        )
    endif()
endif()
