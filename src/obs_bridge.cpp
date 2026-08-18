// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "obs_bridge.h"
#include "tree_dock.h"
#include <QInputDialog>
#include <QLineEdit>
#include <climits>
#include <vector>
#include <obs-module.h>
#include <obs.h>
#include <obs.hpp>
#include <plugin-support.h>
#include <util/config-file.h>

static ObsBridge *g_bridge = nullptr;
ObsBridge *ObsBridge::get()
{
	return g_bridge;
}
void ObsBridge::create()
{
	if (!g_bridge)
		g_bridge = new ObsBridge();
}
void ObsBridge::destroy()
{
	delete g_bridge;
	g_bridge = nullptr;
}

namespace {
// inOp 若因提前返回、异常或后续插入的 return 卡在 true，整棵树将永久停止响应 OBS 事件，
// 且症状是「树不更新了」，极难定位回这里（J-2）。RAII 保证离开作用域必然复位，裸赋值配对不给这个保证。
// ponytail: inOp 是裸 bool 不是计数器 —— 两个 OpGuard 同时存活（嵌套一次 OBS 变更触发另一次）
// 会让内层析构提前把 inOp 清成 false，外层还没做完就重新暴露在 SCENE_LIST_CHANGED 下，
// 复现的还是这段注释开头说的"树停止更新"那类症状。
// Task 10 现有三个生产者：createSceneInFolder、duplicateScene、removeSceneWithUndo。逐一确认过它们
// 不会彼此嵌套、也不会嵌套进自身（K-2③）：三者都只由用户交互（工具栏/右键菜单点击）直接触发，
// 谁都不会在自己的 OpGuard 作用域内调用另外两个——它们各自唯一会触发的 SCENE_LIST_CHANGED 只走
// frontendEvent 里的 `if (!inOp) emit needsRebuild()`，而 needsRebuild 连到的 TreeDock::rebuild()
// 只重建 UI（读 store/liveCanvases，不调用任何场景增删 API），selection 恢复时 setCurrentIndex 触发的
// currentChanged 处理器也被 rebuilding_ 挡住不会调用 switchToScene。故仍不改成计数器；
// 若未来某个生产者的实现改成会在守卫内调用另一个生产者，需在那时升级成 int inOpDepth。
struct OpGuard {
	ObsBridge *b;
	explicit OpGuard(ObsBridge *br) : b(br) { b->inOp = true; }
	~OpGuard() { b->inOp = false; }
};

// 粗略统计 toJson() 里的节点数（folder+scene），只用于日志对照，非精确树行数（J-8）。
int countNodes(const QString &json)
{
	return json.count(QLatin1String("\"t\":"));
}
} // namespace

ObsBridge::ObsBridge()
{
	saveTimer_.setSingleShot(true);
	saveTimer_.setInterval(2000);
	connect(&saveTimer_, &QTimer::timeout, [] { obs_frontend_save(); });
	obs_frontend_add_event_callback(&ObsBridge::frontendEvent, this);
	obs_frontend_add_save_callback(&ObsBridge::frontendSaveLoad, this);
	// 最终整支审查 Important：SCENE_LIST_CHANGED 只在 OBS 内建场景列表的行内编辑器里发出，
	// 外部改名（OBS 自身改名 UI 的 undo/redo、obs-websocket、脚本、其他插件、我们自己的
	// renameScene）一律不发任何前端事件，树里的名字会永久卡在旧值——这正是"高亮误改名"
	// 那个 Critical 能被触发的前提（陈旧行 + 用户按 Ctrl+Z）。source_rename 是公开 libobs API，
	// 与私有槽禁令无关。
	// 覆盖范围只到主画布，不是全部来源——上一版注释在这里断言过"无条件覆盖"，是错的。
	// scene_info/group_info 都带 OBS_SOURCE_REQUIRES_CANVAS（libobs/obs-scene.c:1746,1766），
	// 这类 source 的改名走 obs_canvas_rename_source 而非 obs_source_set_name 里发全局信号的
	// 那个分支；obs_canvas_rename_source 只在 canvas->flags & MAIN 时才把 "source_rename"
	// 转发到 obs_get_signal_handler()（libobs/obs-canvas.c:371-395），否则只发所在 canvas
	// 自己的 signal_handler 和该 source 的私有 "rename" 信号，两个我们都没订阅。
	// 这个覆盖范围曾是一条已知限制（副画布场景被外部改名后，树里那一行会停在旧名字上，
	// 要等下一次任意原因触发的 rebuild 才自愈）。自 2026-08-18 起副画布场景根本不进树
	// （见 liveCanvases 的说明），限制随之消失：这里需要覆盖的，恰好就是它能覆盖的那一部分。
	// 保留上面这段机制说明，是因为"为什么只到主画布"不写下来就会被当成疏漏而重新去补。
	signal_handler_connect(obs_get_signal_handler(), "source_rename", &ObsBridge::sourceRenamed, this);
	registerHotkeys(); // 绑定的载入要等用户配置就绪，放在 FINISHED_LOADING
}

ObsBridge::~ObsBridge()
{
	unregisterHotkeys();
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", &ObsBridge::sourceRenamed, this);
	obs_frontend_remove_save_callback(&ObsBridge::frontendSaveLoad, this);
	obs_frontend_remove_event_callback(&ObsBridge::frontendEvent, this);
}

void ObsBridge::markDirty()
{
	saveTimer_.start();
}

void ObsBridge::frontendSaveLoad(obs_data_t *save_data, bool saving, void *ptr)
{
	auto *b = static_cast<ObsBridge *>(ptr);
	// 树以**不透明 JSON 字符串**存取，绝不让 obs_data 解析它。
	// obs_data 的数组只能装对象（libobs/obs-data.c:464-484 对非对象元素直接 continue），
	// 若走 obs_data_create_from_json，我们的 "mru":["uuid",...] 这类标量数组会被静默清空，
	// MRU 功能将永不工作且无声失效。存字符串则零翻译损失，foreign 护栏也真正字节无损。
	if (saving) {
		// 存盘前把每个场景节点的 name 刷成 OBS 当前实时名。必须每次都刷：
		// 场景改名后若不刷，存的是旧名字，复制场景集合时的名字回退会解析失败。
		std::map<QString, QString> uuidToName;
		for (const auto &cv : b->liveCanvases())
			for (const auto &sc : cv.scenes)
				uuidToName[sc.uuid] = sc.name;
		b->store.stampSceneNames(uuidToName);

		obs_data_set_string(save_data, "scene_anchor", b->store.toJson().toUtf8().constData());
		// 顺带把热键绑定写进用户配置。OBS 不提供"绑定被改动"的回调，只能搭车在保存时机上；
		// 卸载时还会再存一次，两处都便宜。
		b->saveHotkeys();
	} else {
		const char *json = obs_data_get_string(save_data, "scene_anchor");
		if (json && *json)
			b->store.fromJson(QString::fromUtf8(json));
		else
			b->store.clear();
	}
}

static void pruneToLive(ObsBridge *b)
{
	b->store.resolveAndPrune(b->liveCanvases()); // 按画布分区解析 + 清除，见 Task 5b
}

void ObsBridge::frontendEvent(enum obs_frontend_event event, void *ptr)
{
	auto *b = static_cast<ObsBridge *>(ptr);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// 热键绑定存在用户全局配置里，此时才保证可读
		b->loadHotkeys();
		[[fallthrough]];
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		pruneToLive(b);
		emit b->needsRebuild();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		b->store.clear();
		emit b->needsRebuild();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		if (!b->inOp)
			emit b->needsRebuild();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		const QString cur = b->currentSceneUuid();
		if (!cur.isEmpty())
			b->silentTreeOp([b, cur] { b->store.touchMru(cur, 5); });
		emit b->sceneStateChanged();
		break;
	}
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		emit b->sceneStateChanged();
		break;
	case OBS_FRONTEND_EVENT_THEME_CHANGED:
		emit b->needsRebuild();
		break;
	default:
		break;
	}
}

// source_rename 是 libobs 全局信号，调用方线程不定——可以是发起改名的脚本/obs-websocket 线程，
// 也可能是 UI 线程（OBS 自己的改名 UI、我们自己的 renameScene）。不能在这里碰任何 Qt 部件：
// 排队一个到 bridge 对象所在线程（UI 线程）的调用，真正的 emit 在那次排队调用里发生。
// 只会收到主画布场景的改名——而树里也只有主画布场景，两者正好吻合，见 connect 调用旁的注释。
void ObsBridge::sourceRenamed(void *ptr, calldata_t *cd)
{
	auto *b = static_cast<ObsBridge *>(ptr);
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!obs_scene_from_source(source))
		return; // 只关心场景改名；普通 source 改名跟树无关，逐个重建太浪费
	QMetaObject::invokeMethod(b, [b] { emit b->needsRebuild(); }, Qt::QueuedConnection);
}

// 只报告主画布。副画布（Aitum Vertical 等插件用 obs_frontend_add_canvas 建的）故意不进树，
// 理由是"切不了就不该占位"：
//
// OBS 32 没有"设置某画布当前场景"的 API —— frontend 层只有 get_canvases / add_canvas /
// remove_canvas。唯一沾边的 obs_canvas_set_channel(cv, 0, scene) 不是出路：OBS 前端自己
// 从不调用它（libobs 里唯一调用点是 obs_set_output_source 内部、且只对主画布，obs.c:1815），
// 而且建画布的插件通常在 channel 0 上挂的是**转场源而非场景**（Aitum Vertical 的
// CanvasDock::SwitchScene 即如此），往里写场景等于顶掉它的转场槽位、画面硬切、
// 让它记的"当前场景"与实际画面失同步。也就是说所谓"通用支持"实为主动破坏。
//
// 个别插件确实开了自己的口子（Aitum ≥1.6.1 在全局 proc handler 上注册了
// aitum_vertical_switch_scene），但那是逐个厂商的适配，做一个不做另一个既不公平也难维护，
// 而这个 dock 的三个动词——找到、整理、切换——对切不了的场景只成立两个半，
// 撑不起一个 UI 分区。副画布的场景在它自己的 dock 里切换，那里也是它们该在的地方。
//
// 主画布必须手工构造，不能指望它出现在 obs_frontend_get_canvases 里 —— 该 API
// （OBSStudioAPI.cpp:665-672）遍历的是 OBSBasic::canvases，而后者只由 AddCanvas 填充，
// 即用户显式创建的额外画布。主画布由 obs_startup() 创建，从不入列。
// 佐证：OBS 自己需要「含主画布的全部画布」时，也是手工 emplace_back(obs_get_main_canvas())
// 再拼上额外画布（MultitrackVideoOutput.cpp:374）。
//
// 返回值同时是 TreeStore::resolveAndPrune 的输入：它按 canvas uuid 保活，未出现的画布
// 其树会被剪除。这正是期望行为——副画布不再参与，其残留树条目随之清理。
std::vector<LiveCanvas> ObsBridge::liveCanvases() const
{
	std::vector<LiveCanvas> out;
	obs_canvas_t *main = obs_get_main_canvas();
	const QString mainUuid = QString::fromUtf8(obs_canvas_get_uuid(main));
	LiveCanvas lc;
	lc.uuid = mainUuid;
	// obs_canvas_get_name(main) 返回的是 libobs 内部标识符 "Main"（obs-canvas.c 创建时写死），
	// OBS 自己的界面从不显示它。名字现在没有展示位（不再有画布标题行），留空即可。
	struct obs_frontend_source_list sl = {};
	obs_frontend_get_scenes(&sl);
	for (size_t j = 0; j < sl.sources.num; j++) {
		obs_source_t *s = sl.sources.array[j];
		obs_canvas_t *sc = obs_source_get_canvas(s);
		const bool isMain = !sc || QString::fromUtf8(obs_canvas_get_uuid(sc)) == mainUuid;
		obs_canvas_release(sc);
		if (isMain)
			lc.scenes.push_back(
				{QString::fromUtf8(obs_source_get_uuid(s)), QString::fromUtf8(obs_source_get_name(s))});
	}
	obs_frontend_source_list_free(&sl);
	out.push_back(std::move(lc));
	obs_canvas_release(main);
	return out;
}

QString ObsBridge::currentSceneUuid() const
{
	obs_source_t *s = obs_frontend_get_current_scene();
	if (!s)
		return {};
	const QString u = QString::fromUtf8(obs_source_get_uuid(s));
	obs_source_release(s);
	return u;
}

QString ObsBridge::currentPreviewUuid() const
{
	if (!obs_frontend_preview_program_mode_active())
		return {};
	obs_source_t *s = obs_frontend_get_current_preview_scene();
	if (!s)
		return {};
	const QString u = QString::fromUtf8(obs_source_get_uuid(s));
	obs_source_release(s);
	return u;
}

// obs_get_source_by_uuid 全局解析（libobs/obs.c:2031-2034 查 obs->data.sources），但下面两个函数
// 底层的 OBSBasic::SetCurrentScene/TransitionToScene 只操作主画布状态（programScene、
// obs_get_output_source(0)、只含主画布场景的 ui->scenes），完全不感知画布。喂副画布场景：
// SetCurrentScene 的高亮更新静默不触发，TransitionToScene 却照样把该场景推上主画布输出 ——
// 轻则状态错位，重则直播切错画面。spec §2 已声明副画布场景不支持切换，守卫放这里而非调用方，
// 保证没有调用方能绕过（R-15）。
bool ObsBridge::isMainCanvasScene(const QString &uuid) const
{
	obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!s)
		return false;
	obs_canvas_t *sc = obs_source_get_canvas(s);
	obs_source_release(s);
	if (!sc)
		return true; // 未挂画布 == 主画布，口径同 liveCanvases()
	obs_canvas_t *main = obs_get_main_canvas();
	const bool isMain = QString::fromUtf8(obs_canvas_get_uuid(sc)) == QString::fromUtf8(obs_canvas_get_uuid(main));
	obs_canvas_release(sc);
	obs_canvas_release(main);
	return isMain;
}

void ObsBridge::switchToScene(const QString &uuid)
{
	if (!isMainCanvasScene(uuid)) {
		obs_log(LOG_WARNING, "switchToScene: refused non-main-canvas scene %s", uuid.toUtf8().constData());
		return;
	}
	obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!s)
		return;
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(s);
	else
		obs_frontend_set_current_scene(s);
	obs_source_release(s);
}

void ObsBridge::transitionToScene(const QString &uuid)
{
	if (!isMainCanvasScene(uuid)) {
		obs_log(LOG_WARNING, "transitionToScene: refused non-main-canvas scene %s", uuid.toUtf8().constData());
		return;
	}
	obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!s)
		return;
	if (obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_current_preview_scene(s);
		obs_frontend_preview_program_trigger_transition();
	} else {
		obs_frontend_set_current_scene(s);
	}
	obs_source_release(s);
}

// ── 全局热键 ────────────────────────────────────────────────────────────────
// 只注册一个：聚焦搜索框。为什么值得有——搜索框拿到焦点后，方向键是在 QLineEdit 里移动
// 光标而不是在结果里移动，所以此前"搜索"只能鼠标点：点框、打字、再点结果。直播时这条路
// 太慢。配上回车切到首个匹配（见 tree_dock.cpp），键盘全程是：热键 → 打两个字 → 回车。
//
// 存取要自己做：OBS 只把**它自己**注册的 frontend 热键写进 profile 的 [Hotkeys] 段
//（frontend/widgets/OBSBasic_Hotkeys.cpp:95-110 的 LoadHotkeyData/LoadHotkey），
// 插件注册的不在其列。存到**用户全局配置**而不是场景集合：按键绑定是界面偏好，
// 跟着集合走的话切集合就换了快捷键，不合预期。
static const char *kHotkeyFocusSearch = "SceneAnchor.FocusSearch";
static const char *kHotkeyCfgKey = "FocusSearchHotkey";

void ObsBridge::focusSearchHotkeyCb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *b = static_cast<ObsBridge *>(data);
	// 热键回调在 OBS 的热键线程上，不能在这里碰 Qt 部件。排队到我们自己的对象所在线程，
	// 与 sourceRenamed 同一套做法；context object 是 b，b 销毁时未投递的事件会被撤回。
	QMetaObject::invokeMethod(b, [b] { emit b->focusSearchRequested(); }, Qt::QueuedConnection);
}

void ObsBridge::registerHotkeys()
{
	if (focusSearchHotkey_ != OBS_INVALID_HOTKEY_ID)
		return;
	focusSearchHotkey_ = obs_hotkey_register_frontend(
		kHotkeyFocusSearch, obs_module_text("SceneAnchor.Hotkey.FocusSearch"), focusSearchHotkeyCb, this);
}

void ObsBridge::unregisterHotkeys()
{
	if (focusSearchHotkey_ == OBS_INVALID_HOTKEY_ID)
		return;
	// 这里**不再**调 saveHotkeys()：析构发生在 obs_shutdown 的模块卸载阶段，
	// 那时热键已被 stop_hotkeys() 释放，save 拿到的只会是 NULL（见 saveHotkeys 内注释）。
	// 正常路径的落盘由 frontendSaveLoad 承担，那时一切都还活着。
	obs_hotkey_unregister(focusSearchHotkey_);
	focusSearchHotkey_ = OBS_INVALID_HOTKEY_ID;
}

void ObsBridge::saveHotkeys() const
{
	if (focusSearchHotkey_ == OBS_INVALID_HOTKEY_ID)
		return;
	// 前端 API 在 OBSBasic::closeEvent 里就被拆掉了（obs_frontend_set_callbacks_internal(nullptr)，
	// 注释原文"让插件无法继续调用"），此后 obs_frontend_get_user_config() 返回 nullptr，
	// 而 config_set_string 内部直接取 &config->sections、没有空指针保护。
	// 本机实测这条路径上 config 仍然有效（写入成功），但既然拆除时机确实早于模块卸载，
	// 这个保护是免费的，不赌。
	config_t *c = obs_frontend_get_user_config();
	if (!c)
		return;

	obs_data_array_t *arr = obs_hotkey_save(focusSearchHotkey_);
	// **必须判空**：obs_shutdown() 里 stop_hotkeys() 排在模块卸载循环之前（libobs/obs.c），
	// 所以析构里调到这儿时热键早已释放，HASH_FIND_HKEY 落空、obs_hotkey_save 返回 NULL
	// （libobs/obs-hotkey.c）。把 NULL 塞进 obs_data_set_array 会序列化成 {"bindings":[]}，
	// 于是**每次正常退出都把用户绑好的键抹成空** —— 实测复现过。
	// 注意 NULL 与"用户主动清空绑定"是两回事：后者返回的是有效的空数组，照常写回。
	if (!arr)
		return;

	obs_data_t *wrap = obs_data_create();
	obs_data_set_array(wrap, "bindings", arr);
	// 绑定是对象数组，不是标量数组，所以走 obs_data 的 JSON 没有 §4 那个静默丢元素的问题。
	config_set_string(c, "SceneAnchor", kHotkeyCfgKey, obs_data_get_json(wrap));
	// 与 setOption/setDoubleClickMode 一致地立刻落盘。OBS 的设置页在保存用户配置之后才
	// 触发我们的 save 回调（OBSBasicSettings.cpp 先 config_save_safe 再 SaveProject），
	// 不自己存的话新绑定只在内存里，异常退出即丢。
	config_save(c);
	obs_data_release(wrap);
	obs_data_array_release(arr);
}

void ObsBridge::loadHotkeys()
{
	if (focusSearchHotkey_ == OBS_INVALID_HOTKEY_ID)
		return;
	config_t *c = obs_frontend_get_user_config();
	if (!c)
		return; // 同 saveHotkeys：前端 API 可能已被拆除
	const char *json = config_get_string(c, "SceneAnchor", kHotkeyCfgKey);
	if (!json || !*json)
		return;
	obs_data_t *wrap = obs_data_create_from_json(json);
	if (!wrap)
		return;
	obs_data_array_t *arr = obs_data_get_array(wrap, "bindings");
	if (arr) {
		obs_hotkey_load(focusSearchHotkey_, arr);
		obs_data_array_release(arr);
	}
	obs_data_release(wrap);
}

QString ObsBridge::doubleClickMode() const
{
	config_t *c = obs_frontend_get_user_config();
	config_set_default_string(c, "SceneAnchor", "DoubleClick", "transition");
	return QString::fromUtf8(config_get_string(c, "SceneAnchor", "DoubleClick"));
}

void ObsBridge::setDoubleClickMode(const QString &mode)
{
	config_t *c = obs_frontend_get_user_config();
	config_set_string(c, "SceneAnchor", "DoubleClick", mode.toUtf8().constData());
	config_save(c);
}

// 布尔选项统一走这一对，和 DoubleClick 存在同一个 [SceneAnchor] 段。
// 全部默认 true = 保持加入选项之前的既有行为，升级的人不会发现东西少了。
bool ObsBridge::option(const char *key, bool def) const
{
	config_t *c = obs_frontend_get_user_config();
	config_set_default_bool(c, "SceneAnchor", key, def);
	return config_get_bool(c, "SceneAnchor", key);
}

void ObsBridge::setOption(const char *key, bool v)
{
	config_t *c = obs_frontend_get_user_config();
	config_set_bool(c, "SceneAnchor", key, v);
	config_save(c);
}

void ObsBridge::treeRestore(const char *data)
{
	auto *b = get();
	if (!b)
		return;
	const QString json = QString::fromUtf8(data);
	b->store.fromJson(json);
	obs_log(LOG_INFO, "treeRestore: tree now has %d node(s)", countNodes(json));
	b->markDirty();
	emit b->needsRebuild();
}

void ObsBridge::applyTreeOp(const char *undoName, const std::function<bool()> &op)
{
	const QString before = store.toJson();
	if (!op())
		return;
	const QString after = store.toJson();
	if (before == after)
		return;
	obs_frontend_add_undo_redo_action(undoName, &ObsBridge::treeRestore, &ObsBridge::treeRestore,
					  before.toUtf8().constData(), after.toUtf8().constData(), false);
	// 仅在操作真的改了树时打一行；失败/无变化（before == after，上面已 return）不打，避免拖拽刷屏（J-8）。
	obs_log(LOG_INFO, "applyTreeOp[%s]: %d -> %d node(s)", undoName, countNodes(before), countNodes(after));
	markDirty();
	emit needsRebuild();
}

void ObsBridge::silentTreeOp(const std::function<void()> &op)
{
	op();
	markDirty();
}

// 目标名已被占用（同名场景已存在）时追加数字后缀，直到唯一。OBS 全局 source 名唯一，故按名字查即可判占用。
static QString uniqueSceneName(const QString &base)
{
	QString name = base;
	int i = 2;
	while (obs_source_t *ex = obs_get_source_by_name(name.toUtf8().constData())) {
		obs_source_release(ex);
		name = base + QStringLiteral(" %1").arg(i++);
	}
	return name;
}

void ObsBridge::createSceneInFolder(const QString &canvas, const NodePath &folder)
{
	bool ok = false;
	const QString entered = QInputDialog::getText(static_cast<QWidget *>(dock),
						      QString::fromUtf8(obs_module_text("SceneAnchor.AddScene")),
						      QString::fromUtf8(obs_module_text("SceneAnchor.SceneName")),
						      QLineEdit::Normal,
						      QString::fromUtf8(obs_module_text("SceneAnchor.NewScene")), &ok);
	if (!ok || entered.trimmed().isEmpty())
		return;
	const QString name = uniqueSceneName(entered.trimmed());

	{
		// 第一个插件自己发起的场景增删：obs_scene_create 会触发 OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED，
		// 守卫住这段让 frontendEvent 里的 `if (!b->inOp) emit needsRebuild()` 不在操作中途重建（J-2）。
		OpGuard guard(this);
		obs_scene_t *scene = obs_scene_create(name.toUtf8().constData());
		if (scene) {
			obs_source_t *src = obs_scene_get_source(scene);
			const QString uuid = QString::fromUtf8(obs_source_get_uuid(src));
			applyTreeOp(obs_module_text("SceneAnchor.Undo.AddScene"),
				    [&] { return store.placeScene(canvas, uuid, folder, INT_MAX); });
			obs_frontend_set_current_scene(src);
			obs_scene_release(scene); // 前端持有自己的引用（obs-websocket 同款处理）
		} else {
			obs_log(LOG_WARNING, "createSceneInFolder: obs_scene_create failed for name '%s'",
				name.toUtf8().constData());
		}
	} // guard 析构，inOp 复位
	emit needsRebuild();
}

void ObsBridge::renameScene(const QString &uuid, const QString &newName)
{
	const QString name = newName.trimmed();
	if (name.isEmpty()) {
		emit needsRebuild();
		return;
	} // 空名：还原显示，不动 OBS
	obs_source_t *self = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!self)
		return;
	obs_source_t *clash = obs_get_source_by_name(name.toUtf8().constData());
	if (clash && clash != self) { // 名字被占：还原显示（原生同款约束）
		obs_source_release(clash);
		obs_source_release(self);
		emit needsRebuild();
		return;
	}
	if (clash)
		obs_source_release(clash);
	// 改名不发 SCENE_LIST_CHANGED（OBSBasic::RenameSources 这条路径不 emit 任何 OBS 前端事件，
	// obs_source_set_name 本身也不发）——这一行过去的注释是错的。行文字已经是用户刚提交的新名字，
	// 不需要为了刷新显示而重建；libobs 的全局 source_rename 信号会异步再触发一次
	// needsRebuild()（见 ObsBridge::sourceRenamed），良性的一次多余重建，不必特意去重。
	obs_source_set_name(self, name.toUtf8().constData());
	obs_source_release(self);
}

void ObsBridge::duplicateScene(const QString &uuid)
{
	obs_source_t *src = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!src)
		return;
	obs_scene_t *scene = obs_scene_from_source(src);
	if (!scene) {
		obs_source_release(src);
		return;
	}
	// 不写成「原名 + 空格 + 后缀」的拼接：那样分隔符与前后顺序都由代码定死，译者改不了
	// （中文里「副本」前是否留空格是排版习惯问题，而有的语言副本标记需要前置）。
	// 用带 %1 的格式串，把整句交给 locale。
	const QString base = QString::fromUtf8(obs_module_text("SceneAnchor.CopyOf"))
				     .arg(QString::fromUtf8(obs_source_get_name(src)));
	const QString name = uniqueSceneName(base);
	{
		// K-2③：用 OpGuard 而非裸 inOp 赋值——obs_scene_duplicate 会触发 SCENE_LIST_CHANGED，
		// 守卫住这段防止中途插一次多余重建；作用域收窄到刚好包住这次 OBS 变更（与
		// createSceneInFolder 同款约束），applyTreeOp 自己的 emit needsRebuild() 不受影响
		// （它是显式调用，不看 inOp）。
		OpGuard guard(this);
		obs_scene_t *dup = obs_scene_duplicate(scene, name.toUtf8().constData(), OBS_SCENE_DUP_REFS);
		if (dup) {
			const QString newUuid = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(dup)));
			// 树内插为原节点兄弟（其后）；原节点未归类则同落未归类（不进 store）
			const auto live = liveCanvases();
			for (const auto &cv : live) {
				if (auto p = store.findScene(cv.uuid, uuid)) {
					NodePath parent(p->begin(), p->end() - 1);
					const int idx = p->back() + 1;
					applyTreeOp(obs_module_text("SceneAnchor.Undo.Duplicate"),
						    [&] { return store.placeScene(cv.uuid, newUuid, parent, idx); });
					break;
				}
			}
			obs_scene_release(dup);
		}
	} // guard 析构，inOp 复位
	obs_source_release(src);
	emit needsRebuild();
}

// fix round 1 Critical：快照必须**深**。obs_save_source 只存场景自身的设置与滤镜，不存其子源。
// 删除场景时 obs_sceneitem_destroy（libobs/obs-scene.c:2387）释放每个 item 对子源的引用，
// 独占子源（没有被别的场景共享）随即被销毁；恢复时 scene_load_item（obs-scene.c:1119-1150）
// 是按 uuid/名字**查找**子源，查不到就打警告静默跳过——于是 Ctrl+Z 回来的是空场景，
// 静默数据丢失伪装成撤销成功，比不提供撤销更糟。OBS 自己的 RemoveSelectedScene
// （frontend/widgets/OBSBasic_Scenes.cpp:329-420）为此也是逐项保存子源。
static bool collectChild(obs_scene_t *, obs_sceneitem_t *item, void *p)
{
	auto *arr = static_cast<obs_data_array_t *>(p);
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, collectChild, arr); // 组内的项同样随场景销毁
	// fix round 2 Critical：递归之后**不要 return**——组容器自身也必须快照。场景快照里保有
	// 该组的摆放条目，恢复时 scene_load_item 按 uuid 查找组源；若组本身没被重建就会静默丢弃
	// 该条目，组连同其摆放从场景中消失，而已快照的组成员会以孤立顶层源的形式复活（内容丢失+泄漏）。
	// OBS 自身 save_undo_source_enum（OBSBasic_Scenes.cpp:293-314）同样是"先递归、再保存自己"。
	obs_source_t *child = obs_sceneitem_get_source(item); // 不增引用
	if (!child)
		return true;
	OBSDataAutoRelease cd = obs_save_source(child);
	if (cd)
		obs_data_array_push_back(arr, cd);
	return true;
}

void ObsBridge::removeSceneWithUndo(const QString &uuid)
{
	obs_source_t *src = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!src)
		return;

	// K-3：快照必须在 obs_source_remove 之前取。"scene" 存场景自身（设置+滤镜），
	// "children" 存其直接/间接（组内）子源的完整快照，undo 时先建后者、再载前者。
	OBSDataAutoRelease root = obs_data_create();
	OBSDataAutoRelease sceneData = obs_save_source(src);
	obs_data_set_obj(root, "scene", sceneData);

	OBSDataArrayAutoRelease children = obs_data_array_create();
	if (obs_scene_t *scene = obs_scene_from_source(src))
		obs_scene_enum_items(scene, collectChild, children.Get());
	obs_data_set_array(root, "children", children);
	obs_log(LOG_INFO, "removeSceneWithUndo: snapshot has %zu child source(s)",
		(size_t)obs_data_array_count(children));

	// obs_data_get_json(root) 的返回值在 root 析构后失效，但 obs_frontend_add_undo_redo_action
	// 会在调用内把它拷贝进自己的存储，这里顺序安全（root 要到函数末尾才析构）。
	obs_frontend_add_undo_redo_action(obs_module_text("SceneAnchor.Undo.RemoveScene"), &ObsBridge::undoRemoveScene,
					  &ObsBridge::redoRemoveScene, obs_data_get_json(root),
					  uuid.toUtf8().constData(), false);
	{
		// K-2③：用 OpGuard 包住这次 OBS 变更，让 SCENE_LIST_CHANGED 不在函数中途插一次重建；
		// 函数末尾统一 emit needsRebuild() 一次。
		OpGuard guard(this);
		// 不清理 store 里的场景节点：僵尸条目会话内保留正是"删除后 Ctrl+Z 场景回到原文件夹"
		// 的实现基础（K-3 / spec §4）——下次 rebuild 的 planProjection 一旦重新看到这个 uuid
		// 活着，就会照旧把它接回原来的树位置，不需要任何"复活"逻辑。
		obs_source_remove(src);
	}
	obs_source_release(src);
	// K-8：场景删除是危险且可撤销的操作，专门打一行日志。
	obs_log(LOG_INFO, "removeSceneWithUndo: removed scene %s (store node kept as zombie for undo)",
		uuid.toUtf8().constData());
	emit needsRebuild();
}

void ObsBridge::undoRemoveScene(const char *data)
{
	OBSDataAutoRelease root = obs_data_create_from_json(data);
	if (!root)
		return;

	// 子源必须先于场景恢复——scene_load_item 在加载场景时按 uuid 查找它们。
	// 仍然存在的子源（被别的场景共享，故未被销毁）跳过，否则会重复创建。
	//
	// 实测 Critical：刚建出来的子源必须**一直持有引用到场景加载完**。obs_load_source 返回的
	// 源引用计数为 1，此刻没有任何摆放条目引用它；就地 obs_source_release 会让计数归零、
	// 源当场销毁，几毫秒后 scene_load_item 便报 "Source XXX not found!"，Ctrl+Z 回来的是空场景
	// （静默数据丢失伪装成撤销成功）。libobs 自己的 obs_load_sources（libobs/obs.c）正是
	// "全部建好 → 逐个 load → 最后统一 release" 三趟，这里照抄它的生命周期。
	// 引用交接：obs_source_load2(scene) 里 scene_load_item 为每个条目取自己的引用，
	// 之后我们再 release 才安全。
	OBSDataArrayAutoRelease children = obs_data_get_array(root, "children");
	const size_t n = children ? obs_data_array_count(children) : 0;
	std::vector<obs_source_t *> held;
	held.reserve(n);
	size_t skipped = 0;
	for (size_t i = 0; i < n; ++i) {
		OBSDataAutoRelease cd = obs_data_array_item(children, i);
		const char *cu = obs_data_get_string(cd, "uuid");
		if (cu && *cu) {
			obs_source_t *alive = obs_get_source_by_uuid(cu);
			if (alive) {
				obs_source_release(alive);
				++skipped;
				continue;
			}
		}
		obs_source_t *c = obs_load_source(cd);
		if (c)
			held.push_back(c);
		else
			obs_log(LOG_WARNING, "undoRemoveScene: obs_load_source failed for child '%s'",
				obs_data_get_string(cd, "name"));
	}
	// 第二趟：全部建好后再 load。组容器的 group_load 同样要按 uuid 找组员，
	// 组员必须已经存在——所以 load 不能和 create 交错。
	for (obs_source_t *c : held)
		obs_source_load2(c);

	OBSDataAutoRelease sceneData = obs_data_get_obj(root, "scene");
	if (sceneData) {
		obs_source_t *s = obs_load_source(sceneData);
		if (s) {
			obs_source_load2(s);
			obs_source_release(s);
		}
	}
	// 第三趟：场景条目已各自持引用，这里的临时引用可以放了。
	for (obs_source_t *c : held)
		obs_source_release(c);
	obs_log(LOG_INFO, "undoRemoveScene: %zu child source(s) restored, %zu skipped(alive)", held.size(), skipped);
}

void ObsBridge::redoRemoveScene(const char *data)
{
	obs_source_t *s = obs_get_source_by_uuid(data);
	if (s) {
		obs_source_remove(s);
		obs_source_release(s);
	}
}

void ObsBridge::copyFilters(const QString &uuid)
{
	copyFiltersUuid_ = uuid;
}

bool ObsBridge::hasCopiedFilters() const
{
	if (copyFiltersUuid_.isEmpty())
		return false;
	obs_source_t *s = obs_get_source_by_uuid(copyFiltersUuid_.toUtf8().constData());
	if (!s)
		return false;
	obs_source_release(s);
	return true;
}

void ObsBridge::pasteFilters(const QString &uuid)
{
	obs_source_t *dst = obs_get_source_by_uuid(uuid.toUtf8().constData());
	obs_source_t *src = obs_get_source_by_uuid(copyFiltersUuid_.toUtf8().constData());
	if (dst && src)
		obs_source_copy_filters(dst, src);
	if (dst)
		obs_source_release(dst);
	if (src)
		obs_source_release(src);
}
