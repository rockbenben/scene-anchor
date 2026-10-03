// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// 量具夹具的共享声明：fake_obs.cpp 定义、dock_render.cpp 填写。
#pragma once
#include <QString>
#include <QStringList>
#include <vector>

struct FakeScene {
	QString uuid;
	QString name;
};

extern std::vector<FakeScene> g_scenes; // 主画布实际拥有的场景（OBS 侧真相）
extern QString g_canvas;
extern QString g_program; // 当前节目场景
extern QString g_preview; // Studio 预览场景，空 = 非 studio
extern bool g_icons;
extern bool g_mru;
extern bool g_selectSwitches;
extern QString g_dclickMode;
extern QStringList g_log; // 记下被调用的 OBS 动作，供量具核对「点下去真的发生了什么」
