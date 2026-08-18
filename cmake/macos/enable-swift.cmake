# 注入到 obs-studio 的 project() 之后执行（经 CMAKE_PROJECT_obs-studio_INCLUDE）。
#
# 为什么需要：OBS 32 在 macOS 上无条件 add_subdirectory(libobs-metal)，而该目标是
# 34 个 .swift 文件构成的纯 Swift 库；但 obs-studio 32.0.2 全树唯一的
# enable_language(Swift) 在 plugins/mac-virtualcam/src/camera-extension/CMakeLists.txt。
# 插件模板编译 libobs 时传 -DENABLE_PLUGINS=OFF，那一句于是永远执行不到，
# 配置阶段直接报 "CMake can not determine linker language for target: libobs-metal"。
#
# 上游模板没撞上是因为它 pin 的 obs-studio 31.1.1 还没有 libobs-metal（Metal 渲染器
# 是 32 才加入的）。任何用这套模板并 pin obs-studio ≥32 的插件都会复现。
#
# 我们并不需要 Metal 渲染器（只要 libobs 与 obs-frontend-api），但那个子目录无开关
# 可关，所以补上语言声明是最小改法。
enable_language(Swift)
