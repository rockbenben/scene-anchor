// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "tree_store.h"

struct RowPlan {
	// Header：一条不可选、不可编辑的分组表头。目前只有一种——未归类尾区之前的那条。
	// 画布表头随着「副画布不进树」一起消失了，但 dock 侧的 flags/落点逻辑一直留着
	// 这一类行的处理（header → 落到该画布根），所以加一种表头不需要动拖放。
	enum Kind { Folder, Scene, Header };
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

// unfiledLabel 非空时，未归类尾区之前会多出一条 Header 行（表头文案由调用方给——
// 本文件不碰 obs_module_text，才能不链 libobs 地被单元测试直接调用）。
std::vector<RowPlan> planProjection(const TreeStore &store, const std::vector<LiveCanvas> &live,
				    const QString &unfiledLabel = QString());
