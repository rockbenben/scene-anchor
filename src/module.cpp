// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "obs_bridge.h"
#include "tree_dock.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("rockbenben")

// 插件管理器列表里这一行的名字。优先级见 obs-module.c 的 obs_get_module_name：
// data/manifest.json 里的 display_name 存在时压过这个 C 导出，所以正常安装下生效的是
// 清单里的 "SceneAnchor"（构建期由 CMake 生成，见根 CMakeLists.txt）。
// 这里保留本地化版本作为兜底——只拷了二进制、没拷 data/ 的安装方式下没有清单，
// 那时若连这个导出也没有，列表里显示的会是二进制文件名。
MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("SceneAnchor.DockTitle");
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	ObsBridge::create();
	auto *dock = new TreeDock();
	ObsBridge::get()->dock = dock;
	obs_frontend_add_dock_by_id("scene_anchor_dock", obs_module_text("SceneAnchor.DockTitle"), dock);
	return true;
}

void obs_module_unload(void)
{
	ObsBridge::destroy(); // dock 由 OBS 前端先行销毁
}
