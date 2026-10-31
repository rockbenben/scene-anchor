// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// 最小 UI 回归：用 fake_obs 装真的 TreeDock，离屏跑搜索状态、提示显隐、
// 新建入口禁用和“点击当前场景不重复切换”。不替代手动发布清单。

#include "fake_obs.h"
#include "obs_bridge.h"
#include "tree_dock.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QModelIndex>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyleHints>
#include <QThread>
#include <QToolButton>
#include <QTreeView>
#include <cstdio>
#include <obs-module.h>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)

static QLabel *labelWithText(TreeDock &dock, const char *key)
{
	const QString text = QString::fromUtf8(obs_module_text(key));
	for (QLabel *l : dock.findChildren<QLabel *>())
		if (l->text() == text)
			return l;
	return nullptr;
}

static QToolButton *buttonWithTooltip(TreeDock &dock, const char *key)
{
	const QString text = QString::fromUtf8(obs_module_text(key));
	for (QToolButton *b : dock.findChildren<QToolButton *>())
		if (b->toolTip() == text)
			return b;
	return nullptr;
}

static QModelIndex findRow(QTreeView *view, const QString &name)
{
	for (QModelIndex i = view->model()->index(0, 0); i.isValid(); i = view->indexBelow(i))
		if (i.data(Qt::DisplayRole).toString() == name)
			return i;
	return {};
}

int main(int argc, char **argv)
{
	const QByteArray repoMacro = SA_REPO_ROOT;
	qputenv("QT_QPA_PLATFORM", "minimal");
	qputenv("QT_PLUGIN_PATH", SA_QT_PLUGIN_PATH);
	std::fprintf(stderr, "stage app\n");
	QApplication app(argc, argv);
	std::fprintf(stderr, "stage fixture\n");

	const QString repo = QString::fromUtf8(repoMacro);
	qputenv("SA_DATA_DIR", (repo + "/data").toUtf8());
	qputenv("SA_LOCALE", (repo + "/data/locale/en-US.ini").toUtf8());

	g_scenes = {{"s1", "Alpha"}, {"s2", "Beta"}, {"s3", "Gamma"}};
	g_canvas = "canvas-main";
	g_program = "s1";
	g_preview.clear();
	g_icons = true;
	g_mru = true;
	g_selectSwitches = true;
	g_dclickMode = "transition";

	auto *bridge = ObsBridge::get();
	std::fprintf(stderr, "stage dock\n");
	TreeDock dock;
	dock.resize(460, 600);
	dock.show();
	QCoreApplication::processEvents();
	std::fprintf(stderr, "stage rebuild\n");
	dock.rebuild();
	QCoreApplication::processEvents();
	std::fprintf(stderr, "stage find\n");

	auto *search = dock.findChild<QLineEdit *>();
	auto *hint = labelWithText(dock, "SceneAnchor.EmptyHint");
	auto *addScene = buttonWithTooltip(dock, "SceneAnchor.AddScene");
	auto *addFolder = buttonWithTooltip(dock, "SceneAnchor.AddFolder");
	CHECK(search);
	CHECK(hint);
	CHECK(addScene);
	CHECK(addFolder);
	if (!search || !hint || !addScene || !addFolder)
		return 1;

	// 初始空文件夹集合：提示可见，新建可用。
	CHECK(hint->isVisible());
	CHECK(addScene->isEnabled());
	CHECK(addFolder->isEnabled());

	// 只匹配未归类表头：提示隐藏、按 NoMatch 显示、新建禁用；禁用点击不落节点。
	search->setText(QStringLiteral("Unfiled"));
	QCoreApplication::processEvents();
	std::fprintf(stderr, "stage header search\n");
	QLabel *noMatch = labelWithText(dock, "SceneAnchor.NoMatch");
	CHECK(noMatch && noMatch->isVisible());
	CHECK(!hint->isVisible());
	CHECK(!addScene->isEnabled());
	CHECK(!addFolder->isEnabled());
	const QString before = bridge->store.toJson();
	addFolder->click();
	QCoreApplication::processEvents();
	CHECK(bridge->store.toJson() == before);

	search->clear();
	QCoreApplication::processEvents();
	CHECK(hint->isVisible());
	CHECK(addScene->isEnabled());
	CHECK(addFolder->isEnabled());

	// 只匹配一个空文件夹：状态是“搜索中不能拖动或新建”，不是 NoMatch。
	CHECK(bridge->store.insertFolder(g_canvas, {}, 0, "EmptyFolder"));
	dock.rebuild();
	QCoreApplication::processEvents();
	std::fprintf(stderr, "stage folder search\n");
	search->setText(QStringLiteral("EmptyFolder"));
	QCoreApplication::processEvents();
	QLabel *filterStatus = labelWithText(dock, "SceneAnchor.FilterNoDrag");
	CHECK(filterStatus && filterStatus->isVisible());
	CHECK(!addScene->isEnabled());
	CHECK(!addFolder->isEnabled());
	search->clear();
	QCoreApplication::processEvents();
	CHECK(addScene->isEnabled());
	CHECK(addFolder->isEnabled());

	// 搜索期间的 expandAll() 不能被清空搜索的那一刻回写进 store：清空后必须回到搜索前的折叠态。
	{
		NodePath parent;
		CHECK(bridge->store.insertFolder(g_canvas, {}, 0, "Parent", &parent));
		CHECK(bridge->store.placeScene(g_canvas, "s3", parent, 0));
		CHECK(bridge->store.setExpanded(g_canvas, parent, false));
		dock.rebuild();
		QCoreApplication::processEvents();
		auto *treeView = dock.findChild<QTreeView *>();
		CHECK(treeView);
		if (treeView) {
			const QModelIndex parentRow = findRow(treeView, QStringLiteral("Parent"));
			CHECK(parentRow.isValid());
			CHECK(!treeView->isExpanded(parentRow));
			search->setText(QStringLiteral("Gamma"));
			QCoreApplication::processEvents();
			CHECK(treeView->isExpanded(parentRow));
			search->clear();
			QCoreApplication::processEvents();
			CHECK(!treeView->isExpanded(parentRow));
		}
	}

	// 切换高亮只碰旧/新两行：600 场景时全表 setFont 是 O(N) 的 dataChanged 风暴。
	auto *view = dock.findChild<QTreeView *>();
	CHECK(view);
	if (!view)
		return 1;
	{
		auto *proxy = qobject_cast<QSortFilterProxyModel *>(view->model());
		CHECK(proxy);
		if (proxy) {
			auto *src = qobject_cast<QStandardItemModel *>(proxy->sourceModel());
			CHECK(src);
			int changes = 0;
			auto conn = QObject::connect(proxy->sourceModel(), &QAbstractItemModel::dataChanged,
						     [&changes] { ++changes; });
			g_program = "s2";
			g_preview.clear();
			emit bridge->sceneStateChanged();
			QCoreApplication::processEvents();
			CHECK(changes <= 2);
			QObject::disconnect(conn);
			if (src) {
				const QModelIndex alpha = findRow(view, QStringLiteral("Alpha"));
				const QModelIndex beta = findRow(view, QStringLiteral("Beta"));
				CHECK(alpha.isValid());
				CHECK(beta.isValid());
				if (alpha.isValid() && beta.isValid()) {
					auto *alphaItem = src->itemFromIndex(proxy->mapToSource(alpha));
					auto *betaItem = src->itemFromIndex(proxy->mapToSource(beta));
					CHECK(alphaItem && !alphaItem->font().bold());
					CHECK(betaItem && betaItem->font().bold());
				}
			}
		}
	}

	// 点击当前节目场景不重复切换；点另一个场景仍切换。
	const QModelIndex alpha = findRow(view, QStringLiteral("Alpha"));
	const QModelIndex beta = findRow(view, QStringLiteral("Beta"));
	CHECK(alpha.isValid());
	CHECK(beta.isValid());
	g_program = "s1";
	g_preview.clear();
	view->clicked(alpha);
	CHECK(g_program == QStringLiteral("s1"));
	std::fprintf(stderr, "stage click alpha\n");
	view->clicked(beta);
	// 单击切换现在等一个系统双击间隔，避免吞掉双击动作。
	{
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
			QThread::msleep(5);
		}
	}
	CHECK(g_program == QStringLiteral("s2"));
	std::fprintf(stderr, "stage click beta\n");
	// MRU 按 uuid 取名的新路径：Beta 切换后应出现在 chip 条里。
	bool mruHasBeta = false;
	for (QToolButton *button : dock.findChildren<QToolButton *>())
		if (button->accessibleName() == QStringLiteral("Beta"))
			mruHasBeta = true;
	CHECK(mruHasBeta);

	// none 模式没有双击动作要保护，单击应立即切换（不用等双击间隔）。
	g_dclickMode = "none";
	g_program = "s2";
	g_preview.clear();
	view->clicked(alpha);
	CHECK(g_program == QStringLiteral("s1"));
	g_dclickMode = "transition";

	if (failures) {
		std::printf("%d FAILURES\n", failures);
		return 1;
	}
	std::printf("all ok\n");
	return 0;
}
