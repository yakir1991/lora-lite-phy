set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(example_tx minimal_tx.cpp)
target_link_libraries(example_tx PRIVATE host_sim_core)

add_executable(example_rx minimal_rx.cpp)
target_link_libraries(example_rx PRIVATE host_sim_core)
