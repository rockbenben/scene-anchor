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
std::vector<RowPlan> planProjection(const TreeStore &store, const std::vector<LiveCanvas> &live,
				    const QString &unfiledLabel)
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
		// 未归类的场景以前是「悄悄排在最后」，与文件夹里的场景同缩进、同图标、同字重，
		// 唯一的区别是位置——用户看不出哪些还没整理（量具实拍确认）。所以给这一段
		// 加一条表头：它不可选、不可编辑，但能作为放置目标（落到画布根）。
		std::vector<RowPlan> unfiled;
		for (const auto &s : cv.scenes)
			if (!consumed.contains(s.uuid))
				unfiled.push_back(
					{RowPlan::Scene, base, s.name, s.uuid, {}, cv.uuid, QString(), false, false});
		if (!unfiled.empty() && !unfiledLabel.isEmpty())
			out.push_back(
				{RowPlan::Header, base, unfiledLabel, QString(), {}, cv.uuid, QString(), false, false});
		for (auto &r : unfiled)
			out.push_back(r);
	}
	return out;
}
