cmake_minimum_required(VERSION 3.22)
project(scene-anchor-tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
add_executable(test_tree_store test_tree_store.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_store.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/projection.cpp)
target_link_libraries(test_tree_store PRIVATE Qt6::Core)

# UI 量具：测各部件最小宽度，不是回归测试，不进 ctest
add_executable(ux_probe EXCLUDE_FROM_ALL ux_probe.cpp)
target_link_libraries(ux_probe PRIVATE Qt6::Widgets)
enable_testing()
add_test(NAME tree_store COMMAND test_tree_store)
