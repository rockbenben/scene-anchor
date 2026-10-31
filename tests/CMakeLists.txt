cmake_minimum_required(VERSION 3.22)
project(scene-anchor-tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
get_filename_component(SA_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
set(SA_QT_PLUGIN_DIR "${SA_QT_PREFIX}/plugins")

# UI 目标编译真的 tree_dock.cpp，需要 OBS 头文件；运行时仍是 fake_obs，不链 libobs。
# 本地构建的头文件在 .deps/include（不进仓库），干净克隆/CI 用 libobs-dev 的 /usr/include/obs。
find_path(
  SA_OBS_INCLUDE_DIR
  NAMES obs.h
  PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/include" "/usr/include/obs"
  NO_DEFAULT_PATH
)
if(NOT SA_OBS_INCLUDE_DIR)
  message(FATAL_ERROR "OBS headers not found. Install libobs-dev or pass -DSA_OBS_INCLUDE_DIR=<dir>.")
endif()
add_executable(
  test_tree_store
  test_tree_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/projection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contrast.cpp
)
# QColor 属于 Gui；仍然不链 Widgets、不链 libobs，所以这个测试在任何装了 Qt 的机器上都能跑
target_link_libraries(test_tree_store PRIVATE Qt6::Core Qt6::Gui)

# UI 量具：测各部件最小宽度，不是回归测试，不进 ctest
add_executable(ux_probe EXCLUDE_FROM_ALL ux_probe.cpp)
target_link_libraries(ux_probe PRIVATE Qt6::Widgets)

# 最小 UI 回归：真的 TreeDock + fake_obs，离屏验证搜索状态、提示显隐、
# 新建禁用和点击当前场景不重复切换。跑在 ctest 里，不依赖 OBS。
add_executable(
  test_dock_ui
  test_dock_ui.cpp
  fake_obs.cpp
  fake_obs.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_dock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_dock.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/obs_bridge.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/projection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contrast.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contrast.h
)
target_include_directories(test_dock_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${SA_OBS_INCLUDE_DIR})
target_compile_definitions(
  test_dock_ui
  PRIVATE SA_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.." SA_QT_PLUGIN_PATH="${SA_QT_PLUGIN_DIR}"
)
target_link_libraries(test_dock_ui PRIVATE Qt6::Widgets)
set_property(TARGET test_dock_ui PROPERTY AUTOMOC ON)

# 实时路径基准：600 场景的 rebuild 与切场景高亮，EXCLUDE_FROM_ALL，不进 ctest。
add_executable(
  bench_dock
  EXCLUDE_FROM_ALL
  bench_dock.cpp
  fake_obs.cpp
  fake_obs.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_dock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_dock.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/obs_bridge.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/tree_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/projection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contrast.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contrast.h
)
target_include_directories(bench_dock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${SA_OBS_INCLUDE_DIR})
target_compile_definitions(bench_dock PRIVATE SA_QT_PLUGIN_PATH="${SA_QT_PLUGIN_DIR}")
target_link_libraries(bench_dock PRIVATE Qt6::Widgets)
set_property(TARGET bench_dock PROPERTY AUTOMOC ON)

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
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contrast.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contrast.h
)
target_include_directories(dock_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${SA_OBS_INCLUDE_DIR})
target_link_libraries(dock_render PRIVATE Qt6::Widgets)
set_property(TARGET dock_render PROPERTY AUTOMOC ON)
enable_testing()
add_test(NAME tree_store COMMAND test_tree_store)
add_test(NAME dock_ui COMMAND test_dock_ui)
set_tests_properties(dock_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")
