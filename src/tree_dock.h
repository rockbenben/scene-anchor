// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "projection.h"
#include <QStandardItemModel>
#include <QTreeView>
#include <QWidget>

class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QHBoxLayout;
class QScrollArea;
class QToolButton;
class QMimeData;

enum AnchorRoles {
	RoleKind = Qt::UserRole + 1, // int(RowPlan::Kind)
	RoleUuid,                    // QString
	RolePath,                    // QVariantList<int>
	RoleCanvas,                  // QString
	RolePlaced,                  // bool
	RoleColor,                   // QString "#rrggbb"，用于给该行的图标染色
	RoleHasKids,                 // bool，仅 Folder 行：里面有没有内容。构建期算好，
				     // 因为 drawBranches 需要它，而在绘制路径里查模型不安全（见 .cpp）
};

class AnchorModel : public QStandardItemModel {
	Q_OBJECT
public:
	using QStandardItemModel::QStandardItemModel;
	// mime 格式 application/x-scene-anchor：JSON 数组 [{"canvas":..,"placed":bool,"path":[..],"uuid":..}]，视图序（文档序，非选择序）
	QStringList mimeTypes() const override;
	QMimeData *mimeData(const QModelIndexList &indexes) const override;
	bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
			     const QModelIndex &parent) const override;
	// 恒返回 false：applyTreeOp 已触发全量重建，不让 Qt 再自己搬行（J-3）
	bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
			  const QModelIndex &parent) override;
	Qt::DropActions supportedDropActions() const override;
};

class TreeDock : public QWidget {
	Q_OBJECT
public:
	TreeDock();
public slots:
	void rebuild();
private slots:
	void onSceneStateChanged();
	void onContextMenu(const QPoint &pos);

private:
	QStandardItem *itemAtSourceIndex(const QModelIndex &proxyIdx) const; // proxy → source item
	NodePath pathOfItem(const QStandardItem *it) const;
	void refreshMru(); // store.mru() → mruBar_ 里的可点击 chips（只做主画布）
	// chip 宽度按可用宽度均分，dock 被拖宽/拖窄后需要重算，否则要么留白要么冒出滚动条。
	// 用宽度差阈值挡住抖动，也顺带杜绝 refreshMru → 建 chip → 再触发 resize 的回环。
	void resizeEvent(QResizeEvent *e) override;
	void updateHintCap();     // 空状态提示的限高随 dock 高度走，放不下则整段隐藏，见 .cpp
	bool hintWanted_ = false; // 树里没有文件夹 = 想显示提示；能不能真显示由 updateHintCap 定
	int mruWidth_ = -1;
	// fix round 1 Important：右键菜单的动作要到 QMenu::exec() 的嵌套事件循环里点击才执行，
	// 菜单开着的这段时间任何 OBS 事件（主题切换、脚本/热键建删场景……直播场景下都不算罕见）
	// 都可能经 needsRebuild() 触发 rebuild()，而 rebuild() 会 removeRows 摧毁全部 item/index。
	// 菜单构建时捕获的 QStandardItem*/QModelIndex 到点击时可能已经是悬空指针/失效索引。
	// 改名动作按身份（uuid 或 canvas+path）在点击时重新定位，找不到就是那一行已经不在了，静默无操作。
	QModelIndex findSceneIndex(const QString &uuid) const;
	QModelIndex findFolderIndex(const QString &canvas, const NodePath &path) const;
	QLineEdit *search_ = nullptr;
	QLabel *hint_ = nullptr;           // 无文件夹时的空状态引导，建了第一个就自动隐藏
	QWidget *mruBar_ = nullptr;        // chips 的宿主，装在 mruScroll_ 里
	QScrollArea *mruScroll_ = nullptr; // 切断 chip 宽度对 dock 最小宽度的传导
	QHBoxLayout *mruLayout_ = nullptr;
	QTreeView *view_ = nullptr;
	AnchorModel *model_ = nullptr;
	QSortFilterProxyModel *proxy_ = nullptr;
	QToolButton *btnAddScene_ = nullptr, *btnAddFolder_ = nullptr, *btnRemove_ = nullptr;
	bool rebuilding_ = false;
	// J-7 改名缺口（K-2②）：itemChanged 里改名一个 Folder 后，紧接着触发的这次 rebuild() 要用
	// "刚提交的新名字"去匹配选中恢复，而不是重新信任 it->text()（改名与嵌套重建之间的时序不保证
	// 总是已经生效）。改名操作把目标 (canvas, path, newName) 记在这里，rebuild() 消费一次后清空，
	// 不匹配的后续 rebuild 不受影响。path 不变是前提——renameFolder 只改名字不挪动位置。
	QString pendingRenameCanvas_;
	NodePath pendingRenamePath_;
	QString pendingRenameName_;
	bool hasPendingRename_ = false;
};
