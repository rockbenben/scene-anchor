cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/module.cpp src/obs_bridge.cpp src/tree_dock.cpp src/tree_store.cpp src/projection.cpp
)

# OBS 32 插件清单（libobs/obs-module.c 的 obs_module_load_metadata 会读插件 data 目录下的
# manifest.json）。填了它，obs_get_module_id / obs_get_module_version 才有返回值——32.2 的
# 插件管理器只渲染名字，这两个字段是存而未显，属于为后续版本铺路。
#
# display_name 必须写：obs_get_module_name 一旦发现 metadata 存在就优先取 metadata->display_name，
# 而缺失的 JSON 键经 obs_data_get_string 得到的是空串（非 NULL），会让插件管理器回落到显示
# 二进制文件名。也正因为它压过 C 导出的 obs_module_name()，这一项无法本地化——
# 品牌名保持英文，界面里那个可本地化的"场景锚点"仍由 dock 标题承担。
#
# 生成而非静态文件：版本号与 name/id/website 全部取自 buildspec.json，避免发版时漏改。
string(JSON _display_name GET ${buildspec} displayName)
set(_description "Scene folders that stay put.")
configure_file(cmake/common/resources/manifest.json.in "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" @ONLY)
target_add_resource(${CMAKE_PROJECT_NAME} "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
