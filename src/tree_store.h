// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <QSet>
#include <QString>
#include <QStringList>
#include <map>
#include <memory>
#include <optional>
#include <vector>

struct TreeNode {
	enum Type { Folder, Scene };
	Type type = Folder;
	QString name;  // Folder：文件夹名。Scene：持久化的场景名，仅作 uuid 失效时的回退解析器（见 resolveAndPrune）
	QString uuid;  // Scene 专用
	QString color; // "" 或 "#rrggbb"
	bool expanded = true;                            // Folder 专用
	std::vector<std::unique_ptr<TreeNode>> children; // Folder 专用
};

using NodePath = std::vector<int>; // 从 canvas 根出发的子索引链；{} = 根
inline constexpr int kTreeStoreVersion = 1;

// OBS 当前实际拥有的场景，按 canvas 分组。TreeStore 的加载期解析与 planProjection 都消费它。
// 必须按 canvas 分组而非拉平：场景可经 obs_canvas_move_scene 换画布，拉平会让原画布
// 的节点因「uuid 仍活着」而永久留存成幽灵；名字回退拉平则会让一个画布偷走另一个画布的场景。
struct LiveScene {
	QString uuid;
	QString name;
};
struct LiveCanvas {
	QString uuid;
	QString name;
	std::vector<LiveScene> scenes;
};

class TreeStore {
public:
	// 变更器：返回 false = 操作结构非法（坏路径等），store 未动。
	// 是否"真的变了"由调用方比较 toJson() 前后判断（ObsBridge::applyTreeOp）。
	bool insertFolder(const QString &canvas, const NodePath &parent, int index, const QString &name);
	bool renameFolder(const QString &canvas, const NodePath &path, const QString &name);
	bool dissolveFolder(const QString &canvas, const NodePath &path); // 子项上移到原位置
	bool removeNode(const QString &canvas, const NodePath &path);     // 节点+子树移出 store（不碰 OBS 场景）
	// insertedAt / movedCount 为出参：moveNodes 会自行下调插入点（同父且在目标之前的项被移走时），
	// 且祖先吞并会让实际移动数少于传入的 sources 数。调用方若要在这批之后继续插入，
	// 必须用这两个真实值，不能拿原始 destIndex 与 sources.size() 推算 —— 那正是 R-19 的成因。
	bool moveNodes(const QString &canvas, std::vector<NodePath> sources, const NodePath &destFolder, int destIndex,
		       int *insertedAt = nullptr, int *movedCount = nullptr);
	bool placeScene(const QString &canvas, const QString &sceneUuid, const NodePath &destFolder, int destIndex);
	bool setColor(const QString &canvas, const NodePath &path, const QString &color);
	bool setExpanded(const QString &canvas, const NodePath &path, bool expanded);

	const TreeNode *nodeAt(const QString &canvas, const NodePath &path) const; // {} → 隐式根
	std::optional<NodePath> findScene(const QString &canvas, const QString &sceneUuid) const;
	const std::vector<std::unique_ptr<TreeNode>> *canvasRoot(const QString &canvas) const; // 无此 canvas → nullptr

	// canvas 标题行的展开态。存在 canvas 条目上而非节点上 —— 它不是树中的节点。
	// 不持久化的话，用户折叠某个画布分组后，下一次 rebuild（改名/切主题/切集合）就会弹回展开。

	void touchMru(const QString &sceneUuid, int cap);
	QStringList mru() const { return mru_; }

	// 保存前由 ObsBridge 调用，把每个场景节点的 name 刷成 OBS 当前实时名。
	// 必须每次保存都刷：场景被改名后若不刷，存的是旧名字，回退会解析失败。
	void stampSceneNames(const std::map<QString, QString> &uuidToName);

	// 仅加载时调用：先解析后清除。**按 canvas 分区**，不接受拉平的活跃集
	//（拉平会让跨画布移动的场景在原画布留下幽灵，也会让一个画布的名字回退偷走另一画布的场景）。
	//   uuid 命中本画布活跃集 → 保留
	//   uuid 未命中但 name 命中本画布 → 就地改写为新 uuid（自愈，复制集合幸存）
	//   两者皆未命中 → 清除
	void resolveAndPrune(const std::vector<LiveCanvas> &live);
	QString toJson() const;             // foreign 态返回原始串原样
	bool fromJson(const QString &json); // 解析失败 → 清空返回 false；version 更高 → foreign 态返回 true
	bool isForeign() const { return foreign_; }
	void clear();

private:
	TreeNode *
	mutableNodeAt(const QString &canvas,
		      const NodePath &path); // 变更器内部用；仅空路径(根)在 canvas 不存在时建条目，非空路径返回 nullptr
	std::map<QString, TreeNode> roots_;  // canvas uuid → 隐式根（type=Folder）
	QStringList mru_;
	bool foreign_ = false;
	QString rawForeign_;
};
