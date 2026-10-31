// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// 实时路径基准：600 个场景、空树（全部未归类），量 TreeDock::rebuild() 与
// 场景切换高亮的平均耗时。不进 ctest；跑法见 README 的 Build 段。

#include "fake_obs.h"
#include "obs_bridge.h"
#include "tree_dock.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTreeView>
#include <cstdio>

static void process()
{
	QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

int main(int argc, char **argv)
{
	qputenv("QT_QPA_PLATFORM", "minimal");
	qputenv("QT_PLUGIN_PATH", SA_QT_PLUGIN_PATH);
	QApplication app(argc, argv);

	g_canvas = "canvas-main";
	g_program = "s0";
	g_preview.clear();
	g_icons = qgetenv("BENCH_NO_ICONS").isEmpty();
	g_mru = qgetenv("BENCH_NO_MRU").isEmpty();
	g_selectSwitches = true;
	g_dclickMode = "transition";
	for (int i = 0; i < 600; ++i)
		g_scenes.push_back({QString("s%1").arg(i), QString("Scene %1").arg(i + 1)});

	auto *bridge = ObsBridge::get();
	for (int i = 0; i < 5; ++i)
		bridge->store.touchMru(g_scenes[static_cast<size_t>(i)].uuid, 5);

	TreeDock dock;
	dock.resize(460, 600);
	dock.show();
	process();
	dock.rebuild();
	process();

	QElapsedTimer timer;
	constexpr int kRebuilds = 20;
	timer.start();
	for (int i = 0; i < kRebuilds; ++i) {
		dock.rebuild();
		process();
	}
	const qreal rebuildMs = double(timer.nsecsElapsed()) / 1e6 / kRebuilds;

	constexpr int kSwitches = 200;
	timer.restart();
	for (int i = 0; i < kSwitches; ++i) {
		g_program = g_scenes[static_cast<size_t>(i % g_scenes.size())].uuid;
		emit bridge->sceneStateChanged();
		process();
	}
	const qreal switchMs = double(timer.nsecsElapsed()) / 1e6 / kSwitches;

	std::printf("bench_dock: scenes=%zu rebuild=%.2f ms switch=%.2f ms\n", g_scenes.size(), rebuildMs, switchMs);
	return 0;
}
