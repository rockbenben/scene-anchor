// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// 打磨稿量具的假后端：把 src/tree_dock.cpp 真的编译进来，但不链 libobs。
// 这里实现的每一个符号都是 tree_dock.cpp 本来要向 OBS 要的那些，返回值由量具的夹具提供，
// 于是「真的 dock 代码 + 真的 Qt 渲染」可以离屏跑，不需要装 OBS、也不需要动产品代码。
// 不是产品代码，不进 ctest，不进构建产物（EXCLUDE_FROM_ALL）。

#include "fake_obs.h"
#include "obs_bridge.h"
#include "tree_store.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
const char *PLUGIN_NAME = "scene-anchor";
const char *PLUGIN_VERSION = "0.0.0-probe";
}

// ── 夹具：由 dock_render.cpp 填写 ──────────────────────────────────────────
std::vector<FakeScene> g_scenes; // 主画布实际拥有的场景（OBS 侧真相）
QString g_canvas = "canvas-main";
QString g_program; // 当前节目场景
QString g_preview; // Studio 预览场景，空 = 非 studio
bool g_icons = true;
bool g_mru = true;
bool g_selectSwitches = true;
QString g_dclickMode = "transition";
QStringList g_log; // 记下被调用的 OBS 动作，供量具核对「点下去真的发生了什么」

static int sceneIndex(const QString &uuid)
{
	for (size_t i = 0; i < g_scenes.size(); ++i)
		if (g_scenes[i].uuid == uuid)
			return (int)i;
	return -1;
}

// 假 source：obs_source_t* 只是量具内部的身份牌，名字与私有设置各挂一份
struct FakeSrc {
	std::string uuid;
	std::string name;
};
static std::vector<std::unique_ptr<FakeSrc>> g_srcs;
static std::map<std::string, FakeSrc *> g_byUuid;

static FakeSrc *mkSrc(const QString &uuid, const QString &name)
{
	if (!uuid.isEmpty() && g_byUuid.count(uuid.toStdString()))
		return g_byUuid[uuid.toStdString()];
	auto *s = new FakeSrc{uuid.toStdString(), name.toStdString()};
	g_srcs.emplace_back(s);
	if (!uuid.isEmpty())
		g_byUuid[uuid.toStdString()] = s;
	return s;
}

// ── libobs / frontend 假符号 ───────────────────────────────────────────────
extern "C" {

void obs_log(int, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_log << QString::fromUtf8(buf);
}

void blogva(int, const char *fmt, va_list ap)
{
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	g_log << QString::fromUtf8(buf);
}

void bfree(void *p)
{
	std::free(p);
}
void *bmalloc(size_t n)
{
	return std::malloc(n);
}
void *brealloc(void *p, size_t n)
{
	return std::realloc(p, n);
}

obs_module_t *obs_current_module()
{
	return reinterpret_cast<obs_module_t *>(1);
}

static QHash<QByteArray, QByteArray> g_locale;

static void loadLocale()
{
	if (!g_locale.isEmpty())
		return;
	// 每次用时现读：main() 在 QApplication 之前才把 SA_LOCALE 设好，
	// 静态初始化期读到的是空值。
	const QByteArray file = qgetenv("SA_LOCALE");
	const char *f = file.isEmpty() ? "data/locale/en-US.ini" : file.constData();
	FILE *fp = std::fopen(f, "rb");
	if (!fp) {
		std::fprintf(stderr, "locale file not found: %s\n", f);
		return;
	}
	char line[4096];
	while (std::fgets(line, sizeof(line), fp)) {
		char *eq = std::strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		QString k = QString::fromUtf8(line).trimmed();
		QString v = QString::fromUtf8(eq + 1).trimmed();
		if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"'))
			v = v.mid(1, v.size() - 2);
		g_locale.insert(k.toUtf8(), v.toUtf8());
	}
	std::fclose(fp);
}

const char *obs_module_text(const char *val)
{
	loadLocale();
	static QByteArray out;
	out = g_locale.value(QByteArray(val), QByteArray(val));
	return out.constData();
}

bool obs_module_get_string(const char *val, const char **out)
{
	loadLocale();
	if (!g_locale.contains(val))
		return false;
	*out = g_locale.value(val).constData();
	return true;
}

void obs_module_set_locale(const char *) {}
void obs_module_free_locale(void) {}

char *obs_find_module_file(obs_module_t *, const char *file)
{
	const QString root = QString::fromUtf8(qgetenv("SA_DATA_DIR"));
	const QByteArray p = (root + "/" + QString::fromUtf8(file)).toUtf8();
	char *out = (char *)std::malloc(p.size() + 1);
	std::memcpy(out, p.constData(), p.size() + 1);
	return out;
}

char *obs_module_get_config_path(obs_module_t *, const char *file)
{
	return obs_find_module_file(nullptr, file);
}

obs_source_t *obs_get_source_by_uuid(const char *uuid)
{
	const QString u = QString::fromUtf8(uuid ? uuid : "");
	const int i = sceneIndex(u);
	if (i < 0)
		return nullptr;
	FakeSrc *f = mkSrc(u, QString());
	f->name = g_scenes[i].name.toUtf8().constData(); // 名字以夹具为准，改名后不会拿到旧值
	return reinterpret_cast<obs_source_t *>(f);
}

obs_source_t *obs_source_get_source_private(obs_source_t *)
{
	return nullptr;
}

const char *obs_source_get_name(const obs_source_t *s)
{
	auto *f = const_cast<FakeSrc *>(reinterpret_cast<const FakeSrc *>(s));
	return f ? f->name.c_str() : "";
}

void obs_source_set_name(obs_source_t *s, const char *name)
{
	if (auto *f = reinterpret_cast<FakeSrc *>(s))
		f->name = name ? name : "";
}

void obs_source_release(obs_source_t *) {}

struct FakeData {
	std::map<std::string, std::string> str;
	std::map<std::string, long long> num;
	std::map<std::string, bool> bl;
};
static std::map<FakeSrc *, FakeData> g_priv;

obs_data_t *obs_source_get_private_settings(obs_source_t *s)
{
	return reinterpret_cast<obs_data_t *>(&g_priv[reinterpret_cast<FakeSrc *>(s)]);
}
obs_data_t *obs_data_create()
{
	return reinterpret_cast<obs_data_t *>(new FakeData());
}
void obs_data_release(obs_data_t *) {}
void obs_data_addref(obs_data_t *) {}

const char *obs_data_get_string(obs_data_t *d, const char *k)
{
	static QByteArray keep;
	auto *f = reinterpret_cast<FakeData *>(d);
	keep = QString::fromStdString(f->str[std::string(k)]).toUtf8();
	return keep.constData();
}
void obs_data_set_string(obs_data_t *d, const char *k, const char *v)
{
	reinterpret_cast<FakeData *>(d)->str[std::string(k)] = std::string(v ? v : "");
}
long long obs_data_get_int(obs_data_t *d, const char *k)
{
	return reinterpret_cast<FakeData *>(d)->num[std::string(k)];
}
void obs_data_set_int(obs_data_t *d, const char *k, long long v)
{
	reinterpret_cast<FakeData *>(d)->num[std::string(k)] = v;
}
bool obs_data_get_bool(obs_data_t *d, const char *k)
{
	return reinterpret_cast<FakeData *>(d)->bl[std::string(k)];
}
void obs_data_set_bool(obs_data_t *d, const char *k, bool v)
{
	reinterpret_cast<FakeData *>(d)->bl[std::string(k)] = v;
}
void obs_data_set_default_int(obs_data_t *d, const char *k, long long v)
{
	auto *f = reinterpret_cast<FakeData *>(d);
	if (!f->num.count(k))
		f->num[k] = v;
}
void obs_data_set_default_bool(obs_data_t *d, const char *k, bool v)
{
	auto *f = reinterpret_cast<FakeData *>(d);
	if (!f->bl.count(k))
		f->bl[k] = v;
}
void obs_data_set_default_string(obs_data_t *d, const char *k, const char *v)
{
	auto *f = reinterpret_cast<FakeData *>(d);
	if (!f->str.count(k))
		f->str[k] = std::string(v ? v : "");
}

void obs_frontend_open_projector(const char *, int monitor, const char *geometry, const char *name)
{
	g_log << QString("open_projector monitor=%1 geometry=%2 name=%3")
			 .arg(monitor)
			 .arg(QString::fromUtf8(geometry ? geometry : ""))
			 .arg(QString::fromUtf8(name ? name : ""));
}
void obs_frontend_open_source_filters(obs_source_t *s)
{
	g_log << QString("open_source_filters %1").arg(QString::fromUtf8(obs_source_get_name(s)));
}
void obs_frontend_open_source_interaction(obs_source_t *) {}
void obs_frontend_take_source_screenshot(obs_source_t *s)
{
	g_log << QString("take_screenshot %1").arg(QString::fromUtf8(obs_source_get_name(s)));
}
void obs_frontend_get_transitions(obs_frontend_source_list *sources)
{
	// 调用方会走头文件里的 obs_frontend_source_list_free → da_free → free()，
	// 所以这块数组必须是 malloc 出来的，不能是 std::vector 的内部缓冲。
	static const char *names[] = {"Cut", "Fade", "Glide", "Slide", "Stinger"};
	const size_t n = sizeof(names) / sizeof(names[0]);
	sources->sources.array = (obs_source_t **)std::malloc(n * sizeof(obs_source_t *));
	sources->sources.num = n;
	sources->sources.capacity = n;
	for (size_t i = 0; i < n; i++)
		sources->sources.array[i] =
			reinterpret_cast<obs_source_t *>(mkSrc(QString(), QString::fromUtf8(names[i])));
}
} // extern "C"

// ── ObsBridge 假实现 ───────────────────────────────────────────────────────
static ObsBridge *self_ = nullptr;

ObsBridge *ObsBridge::get()
{
	if (!self_)
		create();
	return self_;
}
void ObsBridge::create()
{
	if (!self_)
		self_ = new ObsBridge();
}
void ObsBridge::destroy()
{
	delete self_;
	self_ = nullptr;
}

ObsBridge::ObsBridge() {}
ObsBridge::~ObsBridge() {}

std::vector<LiveCanvas> ObsBridge::liveCanvases() const
{
	LiveCanvas cv;
	cv.uuid = g_canvas;
	cv.name = "Program";
	for (const auto &s : g_scenes)
		cv.scenes.push_back({s.uuid, s.name});
	return {cv};
}

QString ObsBridge::currentSceneUuid() const
{
	return g_program;
}
QString ObsBridge::currentPreviewUuid() const
{
	return g_preview;
}

void ObsBridge::applyTreeOp(const char *undoName, const std::function<bool()> &op)
{
	g_log << QString("applyTreeOp %1").arg(QString::fromUtf8(undoName));
	if (op())
		emit needsRebuild();
}
void ObsBridge::silentTreeOp(const std::function<void()> &op)
{
	op();
}
void ObsBridge::markDirty() {}

void ObsBridge::switchToScene(const QString &uuid)
{
	g_program = uuid;
	store.touchMru(uuid, 5);
	g_log << QString("switch %1").arg(uuid);
	emit sceneStateChanged();
}
void ObsBridge::transitionToScene(const QString &uuid)
{
	g_program = uuid;
	store.touchMru(uuid, 5);
	g_log << QString("transition %1").arg(uuid);
	emit sceneStateChanged();
}
void ObsBridge::createSceneInFolder(const QString &canvas, const NodePath &folder)
{
	const QString uuid = QString("s-new-%1").arg(g_scenes.size());
	g_scenes.push_back({uuid, QString::fromUtf8(obs_module_text("SceneAnchor.NewScene"))});
	applyTreeOp(obs_module_text("SceneAnchor.Undo.AddScene"),
		    [&] { return store.placeScene(canvas, uuid, folder, INT_MAX); });
}
void ObsBridge::duplicateScene(const QString &uuid)
{
	const int i = sceneIndex(uuid);
	if (i < 0)
		return;
	const QString neu = QString::fromUtf8(obs_module_text("SceneAnchor.CopyOf")).replace("%1", g_scenes[i].name);
	const QString id = QString("s-dup-%1").arg(g_scenes.size());
	g_scenes.push_back({id, neu});
	applyTreeOp(obs_module_text("SceneAnchor.Undo.Duplicate"),
		    [&] { return store.placeScene(g_canvas, id, {}, INT_MAX); });
}
void ObsBridge::renameScene(const QString &uuid, const QString &newName)
{
	const int i = sceneIndex(uuid);
	if (i < 0 || newName.trimmed().isEmpty())
		return;
	g_scenes[i].name = newName.trimmed();
	g_log << QString("rename %1 → %2").arg(uuid, newName);
	emit needsRebuild();
}
void ObsBridge::removeSceneWithUndo(const QString &uuid)
{
	g_log << QString("remove %1 (undo registered)").arg(uuid);
}
void ObsBridge::copyFilters(const QString &uuid)
{
	copyFiltersUuid_ = uuid;
	g_log << QString("copyFilters %1").arg(uuid);
}
void ObsBridge::pasteFilters(const QString &uuid)
{
	g_log << QString("pasteFilters → %1").arg(uuid);
}
bool ObsBridge::hasCopiedFilters() const
{
	return !copyFiltersUuid_.isEmpty();
}

QString ObsBridge::doubleClickMode() const
{
	return g_dclickMode;
}
void ObsBridge::setDoubleClickMode(const QString &m)
{
	g_dclickMode = m;
}
bool ObsBridge::option(const char *key, bool) const
{
	const QString k = QString::fromUtf8(key);
	if (k == "ShowMru")
		return g_mru;
	if (k == "SceneIcons")
		return g_icons;
	return g_selectSwitches;
}
void ObsBridge::setOption(const char *key, bool v)
{
	const QString k = QString::fromUtf8(key);
	if (k == "ShowMru")
		g_mru = v;
	else if (k == "SceneIcons")
		g_icons = v;
	else
		g_selectSwitches = v;
}

// 以下符号只为满足 obs_bridge.h 的声明完整性，量具不使用
void ObsBridge::frontendEvent(enum obs_frontend_event, void *) {}
void ObsBridge::frontendSaveLoad(obs_data_t *, bool, void *) {}
void ObsBridge::sourceRenamed(void *, calldata_t *) {}
void ObsBridge::treeRestore(const char *) {}
void ObsBridge::undoRemoveScene(const char *) {}
void ObsBridge::redoRemoveScene(const char *) {}
bool ObsBridge::isMainCanvasScene(const QString &) const
{
	return true;
}
void ObsBridge::registerHotkeys() {}
void ObsBridge::unregisterHotkeys() {}
void ObsBridge::saveHotkeys() const {}
void ObsBridge::loadHotkeys() {}
void ObsBridge::focusSearchHotkeyCb(void *, obs_hotkey_id, obs_hotkey_t *, bool) {}
