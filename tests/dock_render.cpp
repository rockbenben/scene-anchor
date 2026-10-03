// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// 打磨稿量具：把真的 src/tree_dock.cpp 装进真的 Qt 里离屏渲染，输出各状态实拍 + 尺寸台账。
// 主题不是手抄的：直接读 OBS 自己的 Yami.obt / Yami_Light.ovt，按 OBS 的做法把
// var()/calc()/min()/max() 解成具体值，再喂给 setPalette + setStyleSheet；
// 文本直接读仓库自己的 data/locale/*.ini；图标直接用 data/icons/*.svg。
// 于是这张图与 OBS 里那个 dock 的差别只剩「外面没有 OBS 的窗口壳」，而不是「另一个程序的近似」。
//
// 环境变量：SA_REPO 仓库根 / SA_OUT 输出目录 / SA_THEME dark|light|system
//           SA_LANG zh-CN / SA_WIDTHS 320,449 / SA_ICONS 0 / SA_MRU 0 / SA_SCALE 1.5
//           SA_PATCH indent|gap / SA_TEXTDUMP 1
// 不进构建产物，不进 ctest（EXCLUDE_FROM_ALL）。与 tests/ux_probe.cpp 同属量具。

#include "fake_obs.h"
#include "obs_bridge.h"
#include "tree_dock.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QThread>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>
#include <obs-module.h>

namespace {

// ── OBS 主题解析（只覆盖 Yami 实际用到的语法）────────────────────────────
struct Dim {
	double v = 0;
	QString unit; // "" = 无量纲
	QString str;  // 颜色 / 字体名 / url / 关键字
	bool isNum = false;
};

QString dimToStr(const Dim &d)
{
	if (!d.isNum)
		return d.str;
	QString s = QString::number(d.v, 'f', 4);
	while (s.endsWith('0'))
		s.chop(1);
	if (s.endsWith('.'))
		s.chop(1);
	return s + d.unit;
}

using Vars = std::map<QString, Dim>;

Dim valueOf(const QString &name, const Vars &vars)
{
	const auto it = vars.find(name);
	return it == vars.end() ? Dim{} : it->second;
}

Dim toDim(const QString &tok, const Vars &vars, int depth = 0);
QString substVars(const QString &in, const Vars &vars, int depth = 0);

// var(--x) 引用里的名字带 -- 前缀，而表里存的是去掉前缀的名字，两边归一
QString normName(const QString &in)
{
	QString n = in.trimmed();
	while (n.startsWith("--"))
		n.remove(0, 2);
	return n;
}

// var(--x) 解析出来的值本身可能还是表达式，递归展开；depth 挡住主题文件里的自引用
Dim valueOfResolved(const QString &name, const Vars &vars, int depth)
{
	Dim v = valueOf(normName(name), vars);
	// 表里存的是原始串，可能还是数字字面量、别名或表达式；一律交给 toDim 落地
	if (!v.isNum && depth < 24 && !v.str.isEmpty())
		v = toDim(v.str, vars, depth + 1);
	return v;
}

// 词法：数字+量纲 / 函数名 / 括号 / 逗号 / 运算符；其它字符归进字面串
struct Tok {
	QString text;
	Dim num;
	bool isNum = false;
};

std::vector<Tok> lex(const QString &s)
{
	std::vector<Tok> out;
	QString lit;
	auto flush = [&]() {
		if (lit.isEmpty())
			return;
		Tok t;
		t.text = lit;
		static const QRegularExpression numRe("^(-?[0-9.]+)(px|pt|%|em|rem|deg|ms|s)?$");
		if (const auto m = numRe.match(lit); m.hasMatch()) {
			t.num.v = m.captured(1).toDouble();
			t.num.unit = m.captured(2);
			t.num.isNum = true;
			t.isNum = true;
		}
		out.push_back(t);
		lit.clear();
	};
	for (const QChar c : s) {
		if (c == '(' || c == ')' || c == ',') {
			if (lit == "var" || lit == "calc" || lit == "max" || lit == "min" || lit == "url") {
				out.push_back({lit, {}, false});
				lit.clear(); // 忘了清就把 "calc(" 后面的内容粘进函数名里
			} else {
				flush();
			}
			out.push_back({QString(c), {}, false});
			continue;
		}
		if (c == '+' || c == '*' || c == '/') {
			flush();
			out.push_back({QString(c), {}, false});
			continue;
		}
		if (c == '-') { // 负号只在前一个 token 是运算符或开括号时才算符号
			flush();
			out.push_back({QString("-"), {}, false});
			continue;
		}
		if (c.isSpace()) {
			flush();
			continue;
		}
		lit += c;
	}
	flush();
	return out;
}

struct Parser {
	const std::vector<Tok> *toks;
	const Vars *vars;
	int depth = 0;
	size_t i = 0;
	const Tok *peek() const { return i < toks->size() ? &(*toks)[i] : nullptr; }
	Tok take() { return i < toks->size() ? (*toks)[i++] : Tok{}; }

	Dim primary()
	{
		const Tok *t = peek();
		if (!t)
			return {};
		if (t->text == "(") {
			take();
			Dim a = expr();
			if (peek() && peek()->text == ")")
				take();
			return a;
		}
		if (t->text == "calc") { // calc(...) 就是一个带括号的表达式
			take();
			if (peek() && peek()->text == "(")
				take();
			Dim a = expr();
			if (peek() && peek()->text == ")")
				take();
			return a;
		}
		if (t->text == "max" || t->text == "min") {
			const QString fn = take().text;
			if (peek() && peek()->text == "(")
				take();
			Dim best = expr();
			while (peek() && peek()->text == ",") {
				take();
				const Dim nxt = expr();
				if ((fn == "max" && nxt.v > best.v) || (fn == "min" && nxt.v < best.v))
					best = nxt;
			}
			if (peek() && peek()->text == ")")
				take();
			return best;
		}
		if (t->text == "var") {
			take();
			if (peek() && peek()->text == "(")
				take();
			QString name;
			while (peek() && peek()->text != ")")
				name += take().text;
			if (peek() && peek()->text == ")")
				take();
			return valueOfResolved(name, *vars, depth);
		}
		if (t->isNum) {
			take();
			return t->num;
		}
		take();
		Dim d;
		d.str = t->text;
		return d;
	}

	Dim term()
	{
		Dim a = primary();
		while (peek() && (peek()->text == "*" || peek()->text == "/")) {
			const QString op = take().text;
			const Dim b = primary();
			if (op == "*") {
				a.v *= b.v;
				if (a.unit.isEmpty())
					a.unit = b.unit;
			} else if (b.v != 0) {
				a.v /= b.v;
			}
			a.isNum = true;
		}
		return a;
	}

	Dim expr()
	{
		Dim a = term();
		while (peek() && (peek()->text == "+" || peek()->text == "-")) {
			const QString op = take().text;
			const Dim b = term();
			if (a.unit.isEmpty())
				a.unit = b.unit;
			if (a.isNum && b.isNum)
				a.v = (op == "+") ? a.v + b.v : a.v - b.v;
			else if (op == "-")
				a.v = -a.v;
			a.isNum = true;
		}
		return a;
	}
};

Dim toDim(const QString &tok, const Vars &vars, int depth)
{
	const QString t = tok.trimmed();
	if (t.isEmpty() || depth > 24)
		return {};
	static const QRegularExpression varRe("^var\\(\\s*([^)]*?)\\s*\\)$");
	if (const auto m = varRe.match(t); m.hasMatch())
		return valueOfResolved(m.captured(1), vars, depth);
	static const QRegularExpression numRe("^(-?[0-9.]+)(px|pt|%|em|rem|deg|ms|s)?$");
	if (const auto m = numRe.match(t); m.hasMatch()) {
		Dim d;
		d.v = m.captured(1).toDouble();
		d.unit = m.captured(2);
		d.isNum = true;
		return d;
	}
	if (t.startsWith("calc(") || t.startsWith("max(") || t.startsWith("min(")) {
		const std::vector<Tok> toks = lex(t);
		Parser p{&toks, &vars, depth, 0};
		const Dim d = p.expr();
		Dim out = d;
		if (!out.isNum)
			out.str = dimToStr(d);
		return out;
	}
	Dim d;
	d.str = substVars(t, vars, depth);
	return d;
}

QString substVars(const QString &in, const Vars &vars, int depth)
{
	QString out = in;
	static const QRegularExpression varRe("var\\(\\s*([^)]*?)\\s*\\)");
	static const QRegularExpression fnRe("(?:calc|max|min)\\([^()]*(?:\\([^()]*\\)[^()]*)*\\)");
	// 每轮把所有匹配一次换干净（从后往前，偏移才不失效）；轮数只挡自引用死循环
	for (int round = 0; round < 40 && depth <= 24; ++round) {
		int replaced = 0;
		for (const QRegularExpression &re : {varRe, fnRe}) {
			QVector<QRegularExpressionMatch> ms;
			for (auto it = re.globalMatch(out); it.hasNext();)
				ms << it.next();
			for (int i = ms.size() - 1; i >= 0; --i) {
				const auto m = ms[i];
				const QString rep = re == varRe
							    ? dimToStr(valueOfResolved(m.captured(1), vars, depth + 1))
							    : dimToStr(toDim(m.captured(0), vars, depth + 1));
				if (rep == m.captured(0))
					continue;
				out = out.replace(m.capturedStart(), m.capturedLength(), rep);
				++replaced;
			}
		}
		if (!replaced)
			break;
	}
	return out;
}

struct Theme {
	QString qss;
	Vars vars;
};

Theme parseTheme(const QString &ownPath, const QString &parentPath)
{
	std::map<QString, QString> raw;
	QString body;

	auto read = [&](const QString &path) {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly)) {
			std::fprintf(stderr, "!! 主题文件缺失: %s\n", qPrintable(path));
			return;
		}
		const QString txt = QString::fromUtf8(f.readAll());
		static const QRegularExpression varsBlock("@OBSTheme[A-Za-z]+\\s*\\{(.*?)\\n\\}",
							  QRegularExpression::DotMatchesEverythingOption);
		for (auto it = varsBlock.globalMatch(txt); it.hasNext();) {
			static const QRegularExpression def("--([A-Za-z0-9_]+)\\s*:\\s*([^;]*);");
			for (auto d = def.globalMatch(it.next().captured(1)); d.hasNext();) {
				const auto m = d.next();
				raw[m.captured(1)] = m.captured(2).trimmed();
			}
		}
		QString stripped = txt;
		stripped.remove(varsBlock);
		body += stripped;
	};

	if (!parentPath.isEmpty())
		read(parentPath);
	read(ownPath);

	// OBS 从用户配置注入的两个变量：Appearance/FontScale 默认 10，Density 默认 1 → padding 4
	// （obs-studio frontend/OBSApp.cpp:375-376 与 OBSApp_Themes.cpp:789-801）
	raw["obsFontScale"] = "10";
	raw["obsPadding"] = "4";

	Theme th;
	for (const auto &kv : raw)
		th.vars[kv.first] = Dim{0, {}, kv.second, false};
	for (int pass = 0; pass < 30; ++pass) {
		bool changed = false;
		for (auto &kv : th.vars) {
			if (kv.second.isNum)
				continue;
			const QString before = dimToStr(kv.second);
			kv.second = toDim(kv.second.str, th.vars);
			if (dimToStr(kv.second) != before)
				changed = true;
		}
		if (!changed)
			break;
	}
	th.qss = substVars(body, th.vars);
	{ // OBS 的 url(theme:Xxx/a.svg) → 磁盘上的实际路径
		const QString root = QFileInfo(ownPath).absolutePath();
		static const QRegularExpression urlRe("url\\(\\s*theme:([^)]*?)\\s*\\)");
		while (urlRe.match(th.qss).hasMatch()) {
			const auto m = urlRe.match(th.qss);
			th.qss = th.qss.replace(m.capturedStart(), m.capturedLength(),
						QString("url(\"%1/%2\")").arg(root, m.captured(1)));
		}
	}
	return th;
}

QColor parseColor(const QString &s)
{
	static const QRegularExpression rgbRe("^rgb\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*\\)$");
	if (const auto m = rgbRe.match(s); m.hasMatch())
		return QColor(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
	return QColor(s);
}

void applyTheme(const Theme &th, QApplication &app, std::map<QString, QString> *resolved)
{
	// 把解析后的 QSS 落盘，便于核对「量具看到的样式」和「OBS 看到的样式」是否同源
	QFile dump(qgetenv("SA_OUT") + "/theme-resolved.qss");
	if (dump.open(QIODevice::WriteOnly | QIODevice::Truncate))
		dump.write(th.qss.toUtf8());
	QPalette pal = app.palette();
	auto setColor = [&](const char *key, QPalette::ColorGroup grp, QPalette::ColorRole role) {
		const auto it = th.vars.find(QString::fromUtf8(key));
		if (it == th.vars.end())
			return;
		const QColor c = parseColor(it->second.str); // 主题里 rgb(r,g,b) 与 #rrggbb 两种写法都有
		if (c.isValid()) {
			pal.setColor(grp, role, c);
			if (resolved)
				(*resolved)[QString::fromUtf8(key)] = c.name();
		} else if (resolved) {
			(*resolved)[QString::fromUtf8(key)] = QString("<未识别:%1>").arg(it->second.str);
		}
	};
	setColor("palette_window", QPalette::All, QPalette::Window);
	setColor("palette_windowText", QPalette::All, QPalette::WindowText);
	setColor("palette_base", QPalette::All, QPalette::Base);
	setColor("palette_text", QPalette::All, QPalette::Text);
	setColor("palette_highlight", QPalette::All, QPalette::Highlight);
	setColor("palette_highlightedText", QPalette::All, QPalette::HighlightedText);
	setColor("palette_button", QPalette::All, QPalette::Button);
	setColor("palette_buttonText", QPalette::All, QPalette::ButtonText);
	setColor("palette_light", QPalette::All, QPalette::Light);
	setColor("palette_mid", QPalette::All, QPalette::Mid);
	setColor("palette_dark", QPalette::All, QPalette::Dark);
	setColor("palette_text_disabled", QPalette::Disabled, QPalette::Text);
	setColor("palette_text_inactive", QPalette::Inactive, QPalette::Text);
	app.setPalette(pal);
	app.setStyleSheet(th.qss);
}

} // namespace

// ── 夹具 ───────────────────────────────────────────────────────────────────
static void seedStore(TreeStore &store, const QString &cv)
{
	// 空库首屏：一个文件夹都不建，看那条引导提示到底显不显示得下
	if (qgetenv("SA_EMPTY") == "1") {
		store.placeScene(cv, "s-talk", {}, INT_MAX);
		store.placeScene(cv, "s-yy", {}, INT_MAX);
		store.placeScene(cv, "s-screen", {}, INT_MAX);
		for (const char *u : {"s-yy", "s-screen", "s-talk"})
			store.touchMru(QString::fromUtf8(u), 5);
		return;
	}
	const QString folder1 = QStringLiteral("直播"), folder2 = QStringLiteral("开场");
	const QString folder3 = QStringLiteral("录制"), folder4 = QStringLiteral("临时");
	if (QString::fromUtf8(qgetenv("SA_LANG")) == "en-US") {
		// 英文界面用英文文件夹名
	}
	store.insertFolder(cv, {}, 0, folder1);
	store.insertFolder(cv, {0}, 0, folder2);
	store.placeScene(cv, "s-study", {0, 0}, INT_MAX);
	store.placeScene(cv, "s-talk", {0}, INT_MAX);
	store.setColor(cv, {0, 1}, "#d13438");
	store.placeScene(cv, "s-yy", {0}, INT_MAX);
	store.setColor(cv, {0, 2}, "#038387");
	store.insertFolder(cv, {}, 1, folder3);
	store.setExpanded(cv, {1}, false); // 折叠的文件夹
	store.placeScene(cv, "s-screen", {1}, INT_MAX);
	store.placeScene(cv, "s-scrcpy", {1}, INT_MAX);
	store.setColor(cv, {1, 1}, "#c19c00");
	store.insertFolder(cv, {}, 2, folder4); // 空文件夹
	// s-test / s-multi 故意不 placeScene：留在 OBS 里但从没被拖进任何地方，走的才是
	// 投影里那条「未归类尾区」（放在根 ≠ 未归类，planProjection 里不是一回事）。
	for (const char *u : {"s-yy", "s-screen", "s-study", "s-scrcpy", "s-talk"})
		store.touchMru(QString::fromUtf8(u), 5);
}

static void fillScenes(const QString &lang)
{
	g_scenes.clear();
	std::vector<std::pair<QString, QString>> v;
	if (lang == "en-US") {
		v = {{"s-talk", "Commentary Recording - 1080p"},
		     {"s-yy", "YY Live - Remove BG | Add Backdrop"},
		     {"s-screen", "Screen Recording"},
		     {"s-scrcpy", "scrcpy Virtual Camera"},
		     {"s-study", "Study Room - Landscape"},
		     {"s-test", "Test Scene A"},
		     {"s-multi", "Multicam - Wide"}};
	} else {
		v = {{"s-talk", QStringLiteral("解说录制-1080p")}, {"s-yy", QStringLiteral("YY开播-去背景|加底图")},
		     {"s-screen", QStringLiteral("屏幕录制")},     {"s-scrcpy", QStringLiteral("scrcpy 虚拟摄像头")},
		     {"s-study", QStringLiteral("自习室-横屏")},   {"s-test", QStringLiteral("测试场景 A")},
		     {"s-multi", QStringLiteral("多机位-全景")}};
	}
	for (auto &p : v)
		g_scenes.push_back({p.first, p.second});
	g_program = "s-talk";
	g_preview = "s-screen";
}

// ── 渲染 ───────────────────────────────────────────────────────────────────
static QString g_out;
static QStringList g_report;

static void save(const QPixmap &pm, const QString &name)
{
	if (pm.isNull() || pm.width() <= 1) {
		std::fprintf(stderr, "!! 空图: %s\n", qPrintable(name));
		return;
	}
	pm.save(g_out + "/" + name + ".png");
	char buf[160];
	snprintf(buf, sizeof(buf), "%-40s %4dx%4d dpr=%.1f", qPrintable(name), pm.width(), pm.height(),
		 pm.devicePixelRatio());
	g_report << QString::fromUtf8(buf);
}

// 树上第 nth 个指定类型的行，中心点（view 视口坐标，onContextMenu 要的就是这个）
static QPoint centerOf(TreeDock *dock, int kind, int nth = 0)
{
	auto *view = dock->findChild<QTreeView *>();
	if (!view)
		return {};
	auto *m = view->model();
	int seen = 0;
	std::function<QModelIndex(const QModelIndex &)> walk = [&](const QModelIndex &parent) -> QModelIndex {
		for (int r = 0; r < m->rowCount(parent); ++r) {
			const QModelIndex i = m->index(r, 0, parent);
			if (i.data(RoleKind).toInt() == kind && seen++ == nth)
				return i;
			if (const QModelIndex hit = walk(i); hit.isValid())
				return hit;
		}
		return {};
	};
	const QModelIndex hit = walk({});
	if (!hit.isValid())
		return {};
	const QRect r = view->visualRect(hit);
	return QPoint(r.center().x(), r.center().y());
}

static void openMenuAnd(TreeDock *dock, const QPoint &pos, const QString &name, const QString &subTrigger)
{
	g_log.clear();
	bool done = false;
	QTimer::singleShot(80, [&] {
		if (done)
			return;
		done = true;
		QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu) {
			std::fprintf(stderr, "!! 菜单没弹出 %s\n", qPrintable(name));
			return;
		}
		QCoreApplication::processEvents();
		save(menu->grab(), name);
		QStringList items;
		for (QAction *a : menu->actions())
			items << (a->isSeparator() ? QStringLiteral("--")
						   : (a->text().isEmpty() ? QStringLiteral("(submenu)") : a->text()));
		g_report << QString("   menu %1: %2").arg(name, items.join(" | "));
		if (!subTrigger.isEmpty()) {
			QAction *hit = nullptr;
			for (QAction *a : menu->actions())
				if (a->text().contains(subTrigger, Qt::CaseInsensitive) && a->menu())
					hit = a;
			if (!hit) {
				g_report << QString("   !! 没有子菜单项: %1").arg(subTrigger);
				menu->close();
				return;
			}
			QMenu *sm = hit->menu();
			sm->popup(QPoint(40, 40)); // 非模态弹出，可拍
			QCoreApplication::processEvents();
			save(sm->grab(), name + "-" + QString("sub"));
			QStringList si;
			for (QAction *a : sm->actions())
				si << (a->isSeparator() ? QStringLiteral("--") : a->text());
			g_report << QString("   submenu %1: %2").arg(subTrigger, si.join(" | "));
			sm->close();
		}
		menu->close();
	});

	// 探针：拍完关掉，exec() 就返回
	QMetaObject::invokeMethod(dock, "onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, pos));
	// exec() 已经返回时 done 必然为真；万一菜单没弹（invokeMethod 失败/exec 立即返回），
	// 也要给定时器机会把探针跑完，否则 lambda 引用的栈帧就没了
	for (int i = 0; i < 60 && !done; ++i) {
		QCoreApplication::processEvents();
		QThread::msleep(5);
	}
}

// 每个宽度下现取一次台账：菜单/过滤都碰过控件之后再量，数字就不是那个宽度的了
static void metricsAt(TreeDock *dock, int w)
{
	auto *view = dock->findChild<QTreeView *>();
	g_report << QString("--- 宽度 %1（逻辑像素，dpr=%2）---").arg(w).arg(dock->devicePixelRatio(), 0, 'f', 2);
	g_report << QString("  dock minimumSizeHint=%1 sizeHint=%2  实际=%3x%4")
			    .arg(dock->minimumSizeHint().width())
			    .arg(dock->sizeHint().width())
			    .arg(dock->width())
			    .arg(dock->height());
	g_report << QString("  树 行高=%1 缩进=%2 图标=%3 视口宽=%4")
			    .arg(view->sizeHintForRow(0))
			    .arg(view->indentation())
			    .arg(view->iconSize().width())
			    .arg(view->viewport()->width());
	int chips = 0, chipW = 0, chipH = 0;
	QString chipText;
	for (QToolButton *t : dock->findChildren<QToolButton *>()) {
		if (t->text().isEmpty() || !t->isVisible())
			continue; // 有文字的是 MRU chip，按钮行的三个只有图标
		++chips;
		chipW = t->width();
		chipH = std::max(chipH, t->height());
		chipText += QString("[%1 %2x%3@%4,%5 visToParent=%6] ")
				    .arg(t->text())
				    .arg(t->width())
				    .arg(t->height())
				    .arg(t->x())
				    .arg(t->y())
				    .arg(t->isVisibleTo(t->parentWidget()));
	}
	if (auto *scroll = dock->findChild<QScrollArea *>())
		g_report
			<< QString("  MRU 条 高=%1 宽=%2 视口=%3x%4 内容=%5x%6 chip数=%7 chip=%8x%9 滚动条=%10 文本=[%11]")
				   .arg(scroll->height())
				   .arg(scroll->width())
				   .arg(scroll->viewport()->width())
				   .arg(scroll->viewport()->height())
				   .arg(scroll->widget()->width())
				   .arg(scroll->widget()->height())
				   .arg(chips)
				   .arg(chipW)
				   .arg(chipH)
				   .arg(scroll->horizontalScrollBar()->isVisibleTo(scroll) ? "可见" : "隐藏", chipText);
	for (QLabel *l : dock->findChildren<QLabel *>())
		if (l->text().contains(QStringLiteral("文件夹")))
			g_report << QString("  空状态提示 可见=%1 高=%2 需要=%3")
					    .arg(l->isVisible())
					    .arg(l->height())
					    .arg(l->heightForWidth(qMax(1, l->width())));

	// 每行文字放得下吗：可用宽 = 行宽 - 缩进格 - 图标栏；需要宽按真实字体量
	auto *view2 = dock->findChild<QTreeView *>();
	const QFontMetrics fm2(view2->font());
	auto *mm = view2->model();
	std::function<void(const QModelIndex &, int)> rows = [&](const QModelIndex &parent, int depth) {
		for (int r = 0; r < mm->rowCount(parent); ++r) {
			const QModelIndex i = mm->index(r, 0, parent);
			const int kind = i.data(RoleKind).toInt();
			const QString name = i.data(Qt::DisplayRole).toString();
			const int iconCol = g_icons ? view2->iconSize().width() + 4 : 0;
			const int avail = view2->viewport()->width() - view2->indentation() * (depth + 1) - iconCol - 6;
			const int need = fm2.horizontalAdvance(name);
			g_report << QString("    行%1 [%2] 深度=%3 需=%4 可用=%5 %6")
					    .arg(kind == RowPlan::Folder ? "文件夹" : "场景", name)
					    .arg(depth)
					    .arg(need)
					    .arg(avail)
					    .arg(need > avail ? "←放不下" : "ok");
			rows(i, depth + 1);
		}
	};
	rows({}, 0);
	// 搜索框占位符的真实可用宽
	if (auto *se = dock->findChild<QLineEdit *>()) {
		const QFontMetrics fm3(se->font());
		g_report << QString("  搜索框 占位符需=%1 文字区=%2 contentsRect宽=%3 截断=%4")
				    .arg(fm3.horizontalAdvance(se->placeholderText()))
				    .arg(se->contentsRect().width())
				    .arg(se->contentsRect().width())
				    .arg(fm3.horizontalAdvance(se->placeholderText()) > se->contentsRect().width() - 24
						 ? "是"
						 : "否");
	}
}

// 每行「真的画出来是什么颜色」——不重算产品的对比度调整，直接采样它画进 pixmap 的像素。
// 采出来的值拿去和 Base / Highlight 背景算 WCAG，才是一手证据。
static void dumpRowColors(TreeDock *dock)
{
	auto *view = dock->findChild<QTreeView *>();
	auto *m = view->model();
	const QColor bg = view->palette().color(QPalette::Base);
	const QColor hl = view->palette().color(QPalette::Highlight);
	g_report << QString("  行颜色采样（背景 Base=%1 选中=%2）").arg(bg.name(), hl.name());
	std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
		for (int r = 0; r < m->rowCount(parent); ++r) {
			const QModelIndex i = m->index(r, 0, parent);
			const QString stored = i.data(RoleColor).toString();
			if (!stored.isEmpty()) {
				const QIcon ic = qvariant_cast<QIcon>(i.data(Qt::DecorationRole));
				const QImage pm =
					ic.pixmap(QSize(16, 16)).toImage().convertToFormat(QImage::Format_ARGB32);
				QColor best;
				double bestSat = -1;
				for (int y = 0; y < pm.height(); ++y)
					for (int x = 0; x < pm.width(); ++x) {
						const QColor c = QColor::fromRgba(pm.pixel(x, y));
						if (c.alpha() < 200)
							continue;
						const double sat = c.saturationF();
						if (sat > bestSat) {
							bestSat = sat;
							best = c;
						}
					}
				g_report << QString("    [%1] 存=%2 画=%3 行=%4")
						    .arg(i.data(Qt::DisplayRole).toString(), stored, best.name(),
							 i.data(RoleKind).toInt() == RowPlan::Folder ? "文件夹"
												     : "场景");
			}
			if (i.data(Qt::ForegroundRole).canConvert<QColor>()) {
				const QColor fg = qvariant_cast<QColor>(i.data(Qt::ForegroundRole));
				if (fg.isValid() && fg != view->palette().color(QPalette::Text))
					g_report << QString("    [%1] 无图标时文字色=%2")
							    .arg(i.data(Qt::DisplayRole).toString(), fg.name());
			}
			walk(i);
		}
	};
	walk({});
}

int main(int argc, char **argv)
{
	const QString root = QString::fromUtf8(qgetenv("SA_REPO"));
	const QString lang = [] {
		QString l = QString::fromUtf8(qgetenv("SA_LANG"));
		return l.isEmpty() ? QStringLiteral("zh-CN") : l;
	}();
	const QString theme = [] {
		QString t = QString::fromUtf8(qgetenv("SA_THEME"));
		return t.isEmpty() ? QStringLiteral("dark") : t;
	}();
	g_out = QString::fromUtf8(qgetenv("SA_OUT"));
	if (g_out.isEmpty()) {
		std::fprintf(stderr, "SA_OUT 必填\n");
		return 2;
	}
	QDir().mkpath(g_out);
	qputenv("SA_DATA_DIR", (root + "/data").toUtf8());
	qputenv("SA_LOCALE", (root + "/data/locale/" + lang + ".ini").toUtf8());

	// 平台：CJK 字形要靠 Windows 的字体匹配，qminimal 下拿不到替代字体（会画成方框）。
	// 默认走 windows 平台并把窗口挪到屏幕外，只为了取到和真机一致的字形度量。
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
		QByteArray p = qgetenv("SA_PLATFORM");
		if (p.isEmpty())
			p = "windows";
		qputenv("QT_QPA_PLATFORM", p);
	}
	if (const QByteArray scale = qgetenv("SA_SCALE"); !scale.isEmpty())
		qputenv("QT_SCALE_FACTOR", scale); // 必须在 QApplication 之前

	QApplication app(argc, argv);
	if (theme != "system") {
		QApplication::setStyle("Fusion");
		const QString td = root + "/.deps/obs-studio-32.0.2/frontend/data/themes";
		std::map<QString, QString> resolved;
		const Theme th = theme == "light" ? parseTheme(td + "/Yami_Light.ovt", td + "/Yami.obt")
						  : parseTheme(td + "/Yami.obt", QString());
		applyTheme(th, app, &resolved);
		g_report << QString("   主题 %1  Base=%2 Text=%3 Window=%4 Highlight=%5")
				    .arg(theme, resolved["palette_base"], resolved["palette_text"],
					 resolved["palette_window"], resolved["palette_highlight"]);
		if (resolved["palette_base"].isEmpty()) {
			std::fprintf(stderr, "!! 主题没解出 palette_base (vars=%zu)\n", th.vars.size());
			for (const char *k : {"palette_base", "bg_base", "grey6"}) {
				auto it = th.vars.find(QString::fromUtf8(k));
				std::fprintf(stderr, "   %s = [%s] num=%d\n", k,
					     qPrintable(it == th.vars.end() ? QStringLiteral("<缺失>")
									    : dimToStr(it->second)),
					     it == th.vars.end() ? -1 : (int)it->second.isNum);
			}
			return 3;
		}
	}

	g_icons = qgetenv("SA_ICONS") != "0";
	g_mru = qgetenv("SA_MRU") != "0";
	g_selectSwitches = qgetenv("SA_SELSW") != "0";
	g_dclickMode = QString::fromUtf8(qgetenv("SA_DC"));
	if (g_dclickMode.isEmpty())
		g_dclickMode = "transition";

	fillScenes(lang);
	auto *b = ObsBridge::get();
	seedStore(b->store, g_canvas);

	auto *dock = new TreeDock();
	dock->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint); // 别在桌面上闪一个带标题栏的窗
	dock->move(-30000, -30000);
	dock->show();
	const int dockH = qEnvironmentVariableIntValue("SA_HEIGHT") > 0 ? qEnvironmentVariableIntValue("SA_HEIGHT")
									: 420;
	dock->resize(320, dockH);
	dock->rebuild();
	QCoreApplication::processEvents();

	const QStringList widths = QString::fromUtf8(qgetenv("SA_WIDTHS")).split(',', Qt::SkipEmptyParts);
	for (const QString &ws : (widths.isEmpty() ? QStringList{"320"} : widths)) {
		const int w = ws.toInt();
		dock->resize(w, dockH);
		QCoreApplication::processEvents();
		dock->rebuild();
		QCoreApplication::processEvents();
		if (const QByteArray patch = qgetenv("SA_PATCH"); !patch.isEmpty()) {
			if (patch == "indent")
				dock->findChild<QTreeView *>()->setIndentation(20);
			else if (patch == "gap")
				dock->layout()->setSpacing(4);
			else if (patch.startsWith("qss:"))
				dock->setStyleSheet(QString::fromUtf8(patch).mid(4).replace('_', ' '));
			QCoreApplication::processEvents();
		}
		const QString tag = QString("%1_%2_%3%4_w%5%6")
					    .arg(lang, theme, g_icons ? "icons" : "noicons")
					    .arg(g_mru ? QString() : QStringLiteral("_nomru"))
					    .arg(w)
					    .arg(qgetenv("SA_EMPTY") == "1" ? "_empty" : QString());
		// refreshMru 用 deleteLater 摘旧 chip，不冲掉 DeferredDelete 就会把上一轮宽度的
		// 残留 chip 一起数进来（台账会虚高）
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		dock->layout()->activate(); // 不激活就量到上一次 resize 的中间态
		metricsAt(dock, w);
		if (w == 320)
			dumpRowColors(dock);
		save(dock->grab(), tag);

		if (w != 320)
			continue;
		// 状态：搜索无结果
		if (auto *search = dock->findChild<QLineEdit *>()) {
			search->setText(QStringLiteral("zzz无匹配"));
			QCoreApplication::processEvents();
			save(dock->grab(), tag + "_filter-empty");
			search->setText(QStringLiteral("录制"));
			QCoreApplication::processEvents();
			save(dock->grab(), tag + "_filter-hit");
			search->clear();
			QCoreApplication::processEvents();
		}
		// 菜单三兄弟 + 子菜单
		openMenuAnd(dock, centerOf(dock, RowPlan::Scene), tag + "_menu-scene",
			    obs_module_text("SceneAnchor.Menu.Color"));
		openMenuAnd(dock, centerOf(dock, RowPlan::Scene), tag + "_menu-scene-transition",
			    obs_module_text("SceneAnchor.Menu.TransitionOverride"));
		openMenuAnd(dock, centerOf(dock, RowPlan::Scene), tag + "_menu-scene-projector",
			    obs_module_text("SceneAnchor.Menu.FullscreenProjector"));
		// 颜色子菜单要拍在**带色的那一行**上，才能验「当前色描边环」画没画出来
		openMenuAnd(dock, centerOf(dock, RowPlan::Scene, 1), tag + "_menu-scene-colored",
			    obs_module_text("SceneAnchor.Menu.Color"));
		openMenuAnd(dock, centerOf(dock, RowPlan::Folder), tag + "_menu-folder", QString());
		auto *view = dock->findChild<QTreeView *>();
		const QRect vr = view->viewport()->rect();
		openMenuAnd(dock, QPoint(vr.center().x(), vr.bottom() - 2), tag + "_menu-blank",
			    obs_module_text("SceneAnchor.Menu.Display"));
	}

	// ── 台账 ─────────────────────────────────────────────────────────────
	auto *view = dock->findChild<QTreeView *>();
	g_report << QString("=== 台账 %1/%2/%3 ===").arg(lang, theme, g_icons ? "icons" : "noicons");
	g_report << QString("dock minimumSizeHint=%1 sizeHint=%2")
			    .arg(dock->minimumSizeHint().width())
			    .arg(dock->sizeHint().width());
	g_report << QString("树 minimumSizeHint=%1 行高=%2 缩进=%3 图标=%4")
			    .arg(view->minimumSizeHint().width())
			    .arg(view->sizeHintForRow(0))
			    .arg(view->indentation())
			    .arg(view->iconSize().width());
	for (QToolButton *t : dock->findChildren<QToolButton *>()) {
		if (t->text().isEmpty() && !t->icon().isNull())
			g_report << QString("按钮 [%1] sizeHint=%2x%3  图标可用尺寸=%4  实际=%5x%6")
					    .arg(t->toolTip())
					    .arg(t->sizeHint().width())
					    .arg(t->sizeHint().height())
					    .arg(t->icon().availableSizes().isEmpty()
							 ? "无"
							 : QString("%1x%2").arg(
								   t->icon().availableSizes().first().width(),
								   t->icon().availableSizes().first().height()))
					    .arg(t->width())
					    .arg(t->height());
	}
	if (auto *se = dock->findChild<QLineEdit *>())
		g_report << QString("搜索框 sizeHint=%1x%2 实际=%3x%4 样式表根=%5")
				    .arg(se->sizeHint().width())
				    .arg(se->sizeHint().height())
				    .arg(se->width())
				    .arg(se->height())
				    .arg(QApplication::style()->objectName().isEmpty()
						 ? QApplication::style()->metaObject()->className()
						 : QApplication::style()->objectName());
	if (auto *scroll = dock->findChild<QScrollArea *>())
		g_report << QString("MRU 条高=%1 宽=%2 横向滚动条=%3")
				    .arg(scroll->height())
				    .arg(scroll->width())
				    .arg(scroll->horizontalScrollBar()->isVisibleTo(scroll) ? "可见" : "隐藏");
	const QWidget *fontHost = dock->findChild<QLineEdit *>();
	if (!fontHost)
		fontHost = dock;
	g_report << QString("字体 %1 %2pt  dpr=%3")
			    .arg(fontHost->font().family())
			    .arg(fontHost->font().pointSizeF(), 0, 'f', 1)
			    .arg(dock->devicePixelRatio());
	const QColor base = view->palette().color(QPalette::Base);
	const QColor text = view->palette().color(QPalette::Text);
	g_report << QString("Base=%1 Text=%2").arg(base.name(), text.name());

	// 文本宽度台账（真实字体下，每个 locale 每条串要多少像素）
	if (qgetenv("SA_TEXTDUMP") == "1") {
		QFile out(g_out + "/text-metrics.tsv");
		out.open(QIODevice::WriteOnly | QIODevice::Truncate);
		QTextStream ts(&out);
		const QFontMetrics fm(fontHost->font());
		ts << "locale\tkey\tchars\tpx\n";
		for (const QFileInfo &fi : QDir(root + "/data/locale").entryInfoList(QDir::Files)) {
			QFile f(fi.absoluteFilePath());
			f.open(QIODevice::ReadOnly);
			for (const QByteArray &ln : f.readAll().split('\n')) {
				const int eq = ln.indexOf('=');
				if (eq < 0)
					continue;
				QString v = QString::fromUtf8(ln.mid(eq + 1)).trimmed();
				if (v.size() >= 2 && v.startsWith('"'))
					v = v.mid(1, v.size() - 2);
				if (v.endsWith('"'))
					v.chop(1);
				ts << fi.completeBaseName() << '\t' << QString::fromUtf8(ln.left(eq)).trimmed() << '\t'
				   << v.length() << '\t' << fm.horizontalAdvance(v) << '\n';
			}
		}
		g_report << "text-metrics.tsv 已写出";
	}

	{
		QFile out(g_out + "/report.txt");
		out.open(QIODevice::WriteOnly | QIODevice::Append);
		QTextStream ts(&out);
		for (const QString &l : g_report)
			ts << l << '\n';
	}
	for (const QString &l : g_report)
		std::printf("%s\n", qPrintable(l.toUtf8()));
	return 0;
}
