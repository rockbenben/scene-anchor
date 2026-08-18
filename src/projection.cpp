// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "projection.h"
#include <QSet>
#include <map>

static void walkChildren(const std::vector<std::unique_ptr<TreeNode>> &children, int depth, const QString &canvas,
			 const std::map<QString, QString> &liveNames, NodePath &path, QSet<QString> &consumed,
			 std::vector<RowPlan> &out)
{
	for (int i = 0; i < (int)children.size(); ++i) {
		const TreeNode &n = *children[i];
		path.push_back(i);
		if (n.type == TreeNode::Folder) {
			out.push_back(
				{RowPlan::Folder, depth, n.name, QString(), path, canvas, n.color, n.expanded, false});
			walkChildren(n.children, depth + 1, canvas, liveNames, path, consumed, out);
		} else if (auto it = liveNames.find(n.uuid); it != liveNames.end() && !consumed.contains(n.uuid)) {
			out.push_back({RowPlan::Scene, depth, it->second, n.uuid, path, canvas, n.color, false, true});
			consumed.insert(n.uuid);
		}
		path.pop_back();
	}
}

// live 由 ObsBridge::liveCanvases 提供，契约是「恰好一项，即主画布」（副画布故意不进树，
// 理由见那里）。这里仍按 vector 遍历，是为了与 TreeStore::resolveAndPrune 共用同一个入参
// 类型、且 store 本身按画布分区；多于一项时各画布内容会平铺，没有分组表头。
std::vector<RowPlan> planProjection(const TreeStore &store, const std::vector<LiveCanvas> &live)
{
	std::vector<RowPlan> out;
	for (const auto &cv : live) {
		const int base = 0;
		std::map<QString, QString> names;
		for (const auto &s : cv.scenes)
			names[s.uuid] = s.name;
		QSet<QString> consumed;
		if (const auto *root = store.canvasRoot(cv.uuid)) {
			NodePath p;
			walkChildren(*root, base, cv.uuid, names, p, consumed, out);
		}
		for (const auto &s : cv.scenes)
			if (!consumed.contains(s.uuid))
				out.push_back(
					{RowPlan::Scene, base, s.name, s.uuid, {}, cv.uuid, QString(), false, false});
	}
	return out;
}
