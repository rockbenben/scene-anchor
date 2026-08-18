// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "projection.h"
#include "tree_store.h"
#include <QObject>
#include <QTimer>
#include <functional>
#include <obs-frontend-api.h>
#include <obs-hotkey.h>

class TreeDock;

class ObsBridge : public QObject {
	Q_OBJECT
public:
	static ObsBridge *get();
	static void create();
	static void destroy();

	TreeStore store;
	TreeDock *dock = nullptr;

	// 恰好一项：主画布。副画布故意不进树，理由见 .cpp 里的定义处。
	std::vector<LiveCanvas> liveCanvases() const;
	QString currentSceneUuid() const;   // program
	QString currentPreviewUuid() const; // studio preview，非 studio 返回 ""

	// 树操作统一入口：JSON diff → 无变化则丢弃；有变化 → undo 注册 + 防抖保存 + 重建
	void applyTreeOp(const char *undoName, const std::function<bool()> &op);
	// UI 态操作（展开/MRU）：只防抖保存，不进 undo，不触发重建
	void silentTreeOp(const std::function<void()> &op);

	void switchToScene(const QString &uuid);     // studio→设预览；否则设 program。副画布场景无操作（守卫见 .cpp）
	void transitionToScene(const QString &uuid); // studio→设预览+触发转场；否则设 program。同上守卫
	void markDirty();                            // 2s 防抖 → obs_frontend_save()

	// 弹窗要名字 → 建场景（OBS 侧）→ 落进 store 的 folder 末尾（经 applyTreeOp，走 undo）。
	// 插件首次自己发起场景增删：内部用 RAII 守卫扛住中途的 SCENE_LIST_CHANGED（见 .cpp）。
	void createSceneInFolder(const QString &canvas, const NodePath &folder);

	// 双击行为："transition"（默认，studio 下触发转场）｜"rename"｜"none"。存于 OBS 全局用户配置。
	QString doubleClickMode() const;
	void setDoubleClickMode(const QString &mode);
	// 布尔选项。默认值随键名一起定义在 tree_dock.cpp 顶部的 kOpt* 常量里，调用方不再各写一遍。
	bool option(const char *key, bool def) const;
	void setOption(const char *key, bool v);

	// 场景操作组（Task 10）。均在 .cpp 里详细注释各自的 OBS 事件/undo 语义。
	void duplicateScene(const QString &uuid);                      // 复制场景，树内插为兄弟
	void renameScene(const QString &uuid, const QString &newName); // 查重+set_name
	void removeSceneWithUndo(const QString &uuid);                 // 快照+remove+undo 注册（K-3：最危险操作）
	void copyFilters(const QString &uuid);
	void pasteFilters(const QString &uuid);
	bool hasCopiedFilters() const;

	// 全局热键：聚焦搜索框。搜索此前只能靠鼠标——焦点在 QLineEdit 里时方向键是移动光标，
	// 键盘走不到结果上，"快速找到并切换"这个主张因而只兑现了一半。
	// OBS 只自动持久化它自己的 frontend 热键（存 profile 的 [Hotkeys] 段，
	// OBSBasic_Hotkeys.cpp:95-110），插件注册的要自己存取，见 .cpp。
	bool inOp = false; // 自发操作时挡事件重建
signals:
	void needsRebuild();
	void sceneStateChanged();    // program/preview 变化 → 高亮+MRU
	void focusSearchRequested(); // 热键触发，已跨线程排队到 UI 线程
private:
	ObsBridge();
	~ObsBridge() override;
	static void frontendEvent(enum obs_frontend_event event, void *ptr);
	static void frontendSaveLoad(obs_data_t *save_data, bool saving, void *ptr);
	// libobs 全局信号：捕获不经 SCENE_LIST_CHANGED 的主画布场景改名（OBS 自身改名 UI/undo、
	// obs-websocket、脚本、其他插件，以及我们自己的 renameScene）。只覆盖主画布——副画布
	// 场景改名不会到达这里，机制与已知限制见 .cpp 里 connect 调用旁的注释。
	// 可能在 libobs 的任意线程上被调用，见 .cpp 实现。
	static void sourceRenamed(void *ptr, calldata_t *cd);
	static void treeRestore(const char *data);     // undo/redo 共用回调
	static void undoRemoveScene(const char *data); // removeSceneWithUndo 的 undo：obs_load_source 复原
	static void redoRemoveScene(const char *data); // 同上 redo：按 uuid 再次 obs_source_remove
	// uuid 所属场景是否挂在主画布（含未挂画布，视同主画布，与 liveCanvases() 口径一致）。
	// switchToScene/transitionToScene 的守卫：obs_frontend_set_current_scene 等底层只操作主画布状态，
	// 喂副画布场景会导致高亮不更新却仍把该场景推上主画布输出（见 spec §2 / R-15）。
	bool isMainCanvasScene(const QString &uuid) const;
	void registerHotkeys();
	void unregisterHotkeys();
	void saveHotkeys() const;
	void loadHotkeys();
	static void focusSearchHotkeyCb(void *data, obs_hotkey_id id, obs_hotkey_t *key, bool pressed);
	obs_hotkey_id focusSearchHotkey_ = OBS_INVALID_HOTKEY_ID;
	QTimer saveTimer_;
	QString copyFiltersUuid_; // copyFilters 记的源场景 uuid；空 = 未复制
};
