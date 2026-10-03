cmake_minimum_required(VERSION 3.22)
project(scene-anchor-tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
add_executable(
  test_tree_store
  test_tree_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/projection.cpp
)
target_link_libraries(test_tree_store PRIVATE Qt6::Core)

# UI 量具：测各部件最小宽度，不是回归测试，不进 ctest
add_executable(ux_probe EXCLUDE_FROM_ALL ux_probe.cpp)
target_link_libraries(ux_probe PRIVATE Qt6::Widgets)

# 打磨稿量具：把真的 tree_dock.cpp 装进真的 Qt 离屏渲染（假的 OBS 符号在 fake_obs.cpp）。
# 同样不是回归测试，不进 ctest，不进构建产物。
add_executable(
  dock_render
  EXCLUDE_FROM_ALL
  dock_render.cpp
  fake_obs.cpp
  fake_obs.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_dock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_dock.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/obs_bridge.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/projection.cpp
)
target_include_directories(
  dock_render
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../.deps/include
)
target_link_libraries(dock_render PRIVATE Qt6::Widgets)
set_property(TARGET dock_render PROPERTY AUTOMOC ON)
enable_testing()
add_test(NAME tree_store COMMAND test_tree_store)
