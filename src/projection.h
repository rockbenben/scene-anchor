// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "tree_store.h"

struct RowPlan {
	enum Kind { Folder, Scene };
	Kind kind;
	int depth;      // 缩进层级，内容从 0 起
	QString name;   // folder 名 / scene 实时名
	QString uuid;   // scene uuid；folder 为 ""
	NodePath path;  // store 路径；未归类 scene 为空
	QString canvas; // 所属 canvas uuid（store 按画布分区，恒为主画布）
	QString color;
	bool expanded; // Folder：节点展开态
	bool placed;   // scene：true = 来自 store
};

std::vector<RowPlan> planProjection(const TreeStore &store, const std::vector<LiveCanvas> &live);
