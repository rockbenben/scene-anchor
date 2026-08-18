// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "tree_dock.h"
#include "obs_bridge.h"
#include <QColor>
#include <QColorDialog>
#include <QDockWidget>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QShortcut>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>
#include <climits>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>
#include <util/bmem.h>

static const char *kMime = "application/x-scene-anchor";

// Task 12 fix round 1：单一中性灰在 OBS 内建主题上量过 WCAG 对比度不达标——
// #8a8a8a 对 Yami light(#E5E5E5) 背景仅 2.74:1，达不到图形元素 3:1 的门槛
// （Yami dark 4.15:1 达标）。两套主题各配一色：暗色主题 #d0d0d0（对 Yami dark
// 背景 9.29:1，Yami.obt 有明确的 QAbstractItemView 背景规则，Yami_Light.ovt
// extends 它只换 --grey6，两者的树视图背景都已核实），亮色主题 #4a4a4a（对
// Yami light 背景 7.04:1，同样核实过）。以上两个数字是实测，不是推断。
// fix round 3（最终整支审查 Important）：选色不再靠 obs_frontend_is_theme_dark()。
// 该函数返回的是 OBSTheme::isDark，解析自主题文件的 dark: 键；System.obt 没有这个键，
// 该成员留在未初始化状态，返回值不可信。Windows 下 OBS 在 System 主题保留原生 Qt 样式，
// 树视图背景实际跟着系统深浅色模式走的是 Qt 原生 palette，不是主题文件——用
// is_theme_dark() 选色在 System 主题下用的是一个跟真实背景无关的信号，曾经导致
// #4a4a4a 深色字形糊在近黑背景上，约 1.9:1，远低于门槛。
// 现在改问 view_ 实际画在什么背景上：view_->palette().color(QPalette::Base).lightness() < 128
// 判暗——这对 Yami / Yami Light / System 一视同仁，因为问的是背景本身而非猜它属于哪个主题。
// 上面两个对比度数字仍然只对 Yami dark/light 两套背景实测过；System 下的实际背景色没有
// 对应的实测数字可引用，但选择逻辑已经不再依赖"这是不是 System"这个判断本身，
// 这层不确定性不再影响选对哪套图标，只是没有一个具体比值可以写在这里。
static QIcon anchorIcon(const char *file)
{
	char *path = obs_module_file(file);
	QIcon icon(QString::fromUtf8(path ? path : ""));
	bfree(path);
	return icon;
}

static QIcon anchorFolderIcon(bool dark)
{
	return anchorIcon(dark ? "icons/folder_dark.svg" : "icons/folder_light.svg");
}
static QIcon anchorSceneIcon(bool dark)
{
	return anchorIcon(dark ? "icons/scene_dark.svg" : "icons/scene_light.svg");
}
static QIcon anchorPlusIcon(bool dark)
{
	return anchorIcon(dark ? "icons/plus_dark.svg" : "icons/plus_light.svg");
}
static QIcon anchorMinusIcon(bool dark)
{
	return anchorIcon(dark ? "icons/minus_dark.svg" : "icons/minus_light.svg");
}

// MRU chip 的宽度上限。量具 tests/ux_probe.cpp 实测：不设上限时五个中文场景名的 chip
// 自然宽度合计 911px，QHBoxLayout 的最小宽度因而被顶到 915px，并原样传导成整个 dock 的
// 最小宽度——dock 无法被拖窄成侧边栏，且在任何宽度下最右侧 chip 都溢出容器（截图可见）。
// 上限 + 省略号把单个 chip 封顶，滚动区则让整条不再参与 dock 的最小宽度计算。
static constexpr int kMruChipMax = 150;
// 下限：低于此宽度 chip 读不出是哪个场景。真机上 64 太小——297 逻辑像素宽的 dock 里
// 塞下 4 个，每个只剩「自…屏」「sc…像头」这种两头各两字的残段，等于没有。
// 88 起步换来每行少一两个 chip，但每个都认得出。MRU 的价值本来也集中在最前面一两个。
static constexpr int kMruChipMin = 88;

// 三个布尔选项的配置键，存在 OBS 用户配置的 [SceneAnchor] 段，与 DoubleClick 同处。
// 默认全为 true，即加入选项之前的既有行为。
// 默认值与键名绑在一起：读取处和菜单勾选处各写一遍默认值时极易分叉——本项目已经犯过一次
// （图标默认改成 false 后，菜单那侧仍按 true 取，结果图标关着而菜单显示勾选）。
struct Opt {
	const char *key;
	bool def;
};
static constexpr Opt kOptSelectSwitches{"SelectSwitches", true}; // 选中（含方向键）即切换场景
static constexpr Opt kOptShowMru{"ShowMru", true};               // 显示最近使用条
// 树里的图标（文件夹与场景一起开关，不是只管场景——键名 SceneIcons 是历史包袱，
// 不能改，改了已有用户的设置会丢）。默认开，取舍理由见 rebuild 里染色那一段。
static constexpr Opt kOptIcons{"SceneIcons", true};
// 预设色按"好看"挑（Fluent 那一组），不按"在所有背景上都够对比"挑——后者解出来的
// 是一组被逼到 100% 饱和的刺眼色（#f80000 / #927900 / #da00da 之流）。可读性交给下面的
// contrastAdjusted 在绘制时解决：存的是用户选的原色，画的是按当前背景调过明度的版本。
struct Preset {
	const char *hex;
	const char *key; // locale 键后缀
};
static const Preset kColors[8] = {
	{"#d13438", "Red"},  {"#ca5010", "Orange"}, {"#c19c00", "Yellow"}, {"#107c10", "Green"},
	{"#038387", "Teal"}, {"#0078d4", "Blue"},   {"#8764b8", "Purple"}, {"#881798", "Magenta"},
};

static bool presetHas(const QString &hex)
{
	for (const Preset &p : kColors)
		if (hex.compare(QString::fromUtf8(p.hex), Qt::CaseInsensitive) == 0)
			return true;
	return false;
}

// WCAG 2.1 SC 1.4.11：图形元素对背景至少 3:1。
static constexpr double kMinContrast = 3.0;

static double srgbLin(double c)
{
	return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
static double relLum(const QColor &c)
{
	return 0.2126 * srgbLin(c.redF()) + 0.7152 * srgbLin(c.greenF()) + 0.0722 * srgbLin(c.blueF());
}
static double contrastOf(double a, double b)
{
	return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

// 把标签色调到对 bg 达标，只动 HSL 明度、保住色相与饱和度。
// 为什么不改成"一组深浅通吃的固定色"：那样的解存在但窗口极窄（真机实测深色背景
// #272A33、浅色 #E5E5E5，可行亮度只有 L∈[0.17,0.23]），八个色相被压到同一亮度且必须
// 满饱和，非常难看。而且标签色是**用户数据**，按主题换一套值会让同一个场景集合在不同
// 主题下显示成不同颜色。改在绘制期适配，既保住存储值的唯一性，也顺带让用户自选的
// 任意颜色（包括深色背景上的深蓝、浅色背景上的浅黄）自动可读。
static QColor contrastAdjusted(const QColor &c, const QColor &bg)
{
	const double lb = relLum(bg);
	if (contrastOf(relLum(c), lb) >= kMinContrast)
		return c;
	const bool lighten = lb < 0.18; // 0.18 ≈ 中灰亮度：背景比它暗就提亮标签，反之压暗
	float h = 0, s = 0, l = 0, a = 1;
	c.getHslF(&h, &s, &l, &a);
	if (h < 0)
		h = 0; // 无彩色时 Qt 返回 -1，fromHslF 不接受
	double lo = lighten ? l : 0.0, hi = lighten ? 1.0 : l;
	for (int i = 0; i < 24; ++i) {
		const double mid = (lo + hi) / 2;
		const bool ok = contrastOf(relLum(QColor::fromHslF(h, s, float(mid), a)), lb) >= kMinContrast;
		if (lighten)
			(ok ? hi : lo) = mid;
		else
			(ok ? lo : hi) = mid;
	}
	return QColor::fromHslF(h, s, float(lighten ? hi : lo), a);
}

// 颜色菜单的色块。当前所选那一格加一圈高对比描边——**不能靠 QAction 的勾选标记**：
// 带图标的 QAction，勾与图标抢同一列，在 OBS 的样式表下勾会被图标盖掉，结果就是
// "当前色没打勾"（用户实测报告）。同子菜单里「最近使用」「图标」能看到勾，
// 正因为它们没有图标。描边不依赖样式表行为，深浅主题都成立。
static QPixmap colorSwatch(const QColor &raw, const QColor &bg, const QColor &fg, bool current)
{
	QPixmap px(16, 16);
	px.fill(Qt::transparent);
	QPainter p(&px);
	p.setRenderHint(QPainter::Antialiasing, false);
	p.fillRect(QRect(1, 1, 14, 14), contrastAdjusted(raw, bg));
	if (current) {
		p.setPen(QPen(fg, 2));
		p.drawRect(QRect(1, 1, 13, 13));
	}
	p.end();
	return px;
}

// 颜色标签的呈现方式换过三轮，记录下来免得有人再走一遍：
// ① Qt::BackgroundRole 铺满整行 —— 与选中高亮抢同一个视觉通道，带色行看起来像被选中。
// ② 左侧独立色带 —— 近了和图标粘成一坨，远了又飘成孤立的一列；中间没有好位置，
//    而且为了腾出槽位必须两遍绘制（样式表 padding 对自定义 delegate 无效），
//    代价是一整个 delegate 加一套坐标假设，其中一条假设还错了、把带子画到了展开箭头上。
// ③ 现在：直接给该行**已有的图标**染色。不新增任何元素，因而不存在远近问题；
//    一个饱和色图标夹在一列灰图标里非常扎眼，扫视效率反而比细色带高。
//    删掉了 ColorBarDelegate 及其全部坐标常量。
static QIcon tintedIcon(const QIcon &src, const QColor &c)
{
	QPixmap pm = src.pixmap(QSize(16, 16));
	if (pm.isNull())
		return src;
	QPainter p(&pm);
	p.setCompositionMode(QPainter::CompositionMode_SourceIn); // 只染不透明像素，保留字形轮廓
	p.fillRect(pm.rect(), c);
	p.end();
	return QIcon(pm);
}

QStringList AnchorModel::mimeTypes() const
{
	return {QString::fromUtf8(kMime)};
}
Qt::DropActions AnchorModel::supportedDropActions() const
{
	return Qt::MoveAction;
}

QMimeData *AnchorModel::mimeData(const QModelIndexList &indexes) const
{
	// 拖拽结果必须按文档序而非选择序，这正是被替代插件多选拖拽出错的地方（J-3）：
	// 用户随手多选出的 QModelIndexList 顺序不保证跟树上位置一致，直接喂给 moveNodes 会错乱。
	QModelIndexList sorted = indexes;
	std::sort(sorted.begin(), sorted.end(), [](const QModelIndex &a, const QModelIndex &b) {
		QModelIndex x = a, y = b;
		std::vector<int> pa, pb;
		for (; x.isValid(); x = x.parent())
			pa.insert(pa.begin(), x.row());
		for (; y.isValid(); y = y.parent())
			pb.insert(pb.begin(), y.row());
		return pa < pb;
	});
	QJsonArray arr;
	for (const QModelIndex &idx : sorted) {
		if (idx.column() != 0)
			continue;
		QStandardItem *it = itemFromIndex(idx);
		const int kind = it->data(RoleKind).toInt();
		QJsonObject o;
		o["canvas"] = it->data(RoleCanvas).toString();
		o["placed"] = it->data(RolePlaced).toBool() || kind == RowPlan::Folder;
		o["uuid"] = it->data(RoleUuid).toString();
		QJsonArray p;
		for (const QVariant &v : it->data(RolePath).toList())
			p.append(v.toInt());
		o["path"] = p;
		arr.append(o);
	}
	auto *mime = new QMimeData();
	mime->setData(QString::fromUtf8(kMime), QJsonDocument(arr).toJson(QJsonDocument::Compact));
	return mime;
}

// 自绘展开箭头。两个理由，都在真机上量过：
//
// 一、Qt 默认样式画的箭头在深色主题下几乎看不见。OBS 的主题文件里一条 QTreeView /
// ::branch 规则都没有（它自己没有 QTreeView，来源列表是 QListView 手工拼 widget），
// 于是分支指示器落到 Qt 默认实现，用 palette.dark() 上色——深色主题下量到
// rgb(27,27,27) 压 rgb(39,42,51)，对比度 1.20:1；同一行的文字是 12.68。
// 改用调色板的文字色，深浅主题自动跟随。
//
// 二、Qt 只给 hasChildren 为真的行画箭头，于是**空文件夹**没有任何标记。关掉图标后
// 它和场景行长得一模一样，分不出哪个是容器。这里按 RoleKind 画，空文件夹照样有箭头，
// 只是压低透明度表示"里面还没东西"。OBS 的空分组同样保留展开控件
// （SourceTreeItem.cpp:528），口径一致。
class AnchorTreeView : public QTreeView {
public:
	using QTreeView::QTreeView;

protected:
	void drawBranches(QPainter *p, const QRect &rect, const QModelIndex &index) const override
	{
		const int ind = indentation();
		if (ind <= 0 || rect.width() < ind)
			return;
		// rect 是本行左侧的整块分支区，宽度恰好 (层级 + 根装饰) * indentation。
		// 最右一格留给本行自己的展开箭头，左边每一格对应一层祖先。
		const int cells = rect.width() / ind;
		const QColor base = palette().color(QPalette::Text);

		p->save();
		p->setRenderHint(QPainter::Antialiasing, true);

		// 缩进导引线：每层祖先在自己那一格的中线画一条竖线，贯穿整行高度，连成一列。
		// 关掉图标后深度**只能**靠缩进读，而 dock 可以窄到 120 逻辑像素、树可以嵌套多层，
		// 那时"这个场景到底属于哪个文件夹"很难扫。透明度压到 34：扫得见，不抢戏。
		// 不用 Qt 默认那套虚线连接线——它按"有没有后续兄弟"变形，在这个深度只是噪声。
		if (cells > 1) {
			QColor g = base;
			g.setAlpha(34);
			p->setPen(QPen(g, 1.0));
			for (int i = 0; i < cells - 1; ++i) {
				const qreal x = rect.left() + ind * (i + 0.5);
				p->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom() + 1));
			}
		}

		// 展开箭头：只有文件夹行有。
		if (index.data(RoleKind).toInt() == RowPlan::Folder) {
			const QRectF cell(rect.right() + 1 - ind, rect.top(), ind, rect.height());
			const bool hasKids = index.data(RoleHasKids).toBool();
			QColor c = base;
			c.setAlpha(hasKids ? 190 : 110); // 空文件夹压暗：是容器，但里面没东西
			const qreal s = ind * 0.20;      // 半臂长随缩进走，改 indentation 不用改这里
			const QPointF ctr = cell.center();
			QPainterPath path;
			if (isExpanded(index)) { // 展开：˅
				path.moveTo(ctr.x() - s, ctr.y() - s * 0.5);
				path.lineTo(ctr.x(), ctr.y() + s * 0.5);
				path.lineTo(ctr.x() + s, ctr.y() - s * 0.5);
			} else { // 折叠：›
				path.moveTo(ctr.x() - s * 0.5, ctr.y() - s);
				path.lineTo(ctr.x() + s * 0.5, ctr.y());
				path.lineTo(ctr.x() - s * 0.5, ctr.y() + s);
			}
			QPen pen(c, 1.4);
			pen.setCapStyle(Qt::RoundCap);
			pen.setJoinStyle(Qt::RoundJoin);
			p->setPen(pen);
			p->setBrush(Qt::NoBrush);
			p->drawPath(path);
		}
		p->restore();
	}
};

bool AnchorModel::canDropMimeData(const QMimeData *data, Qt::DropAction, int, int, const QModelIndex &parent) const
{
	if (!data->hasFormat(QString::fromUtf8(kMime)))
		return false;
	if (parent.isValid()) {
		QStandardItem *p = itemFromIndex(parent);
		const int k = p->data(RoleKind).toInt();
		if (k == RowPlan::Scene)
			return false; // 场景不是容器
		// 目标 canvas 必须与所有源一致——跨 canvas 移动无安全语义（canDropMimeData 层的第一道拒绝）
		const auto arr = QJsonDocument::fromJson(data->data(QString::fromUtf8(kMime))).array();
		for (const auto &v : arr)
			if (v.toObject()["canvas"].toString() != p->data(RoleCanvas).toString())
				return false;
	}
	return true;
}

bool AnchorModel::dropMimeData(const QMimeData *data, Qt::DropAction, int row, int, const QModelIndex &parent)
{
	const auto arr = QJsonDocument::fromJson(data->data(QString::fromUtf8(kMime))).array();
	if (arr.isEmpty())
		return false;
	auto *b = ObsBridge::get();

	// 目标：folder → 其 store 路径；header/空白 → 该 canvas 根
	QString canvas;
	NodePath destFolder;
	QStandardItem *container = parent.isValid() ? itemFromIndex(parent) : nullptr;
	if (parent.isValid()) {
		canvas = container->data(RoleCanvas).toString();
		if (container->data(RoleKind).toInt() == RowPlan::Folder)
			for (const QVariant &v : container->data(RolePath).toList())
				destFolder.push_back(v.toInt());
		// header → destFolder 留空 = 根
	} else {
		// 视口空白（多 canvas 时 header 恒是顶层行，这条路径基本只有单 canvas 才可能命中）：根，
		// canvas 取拖拽项里第一个（文档序）的 canvas；跨 canvas 项下面静默丢弃（双保险，见 J-3）
		canvas = arr.first().toObject()["canvas"].toString();
	}

	// destIndex 坐标陷阱（本项目同一形状的第三个缺陷，前两个是 J-3/R-19——下一个改这里的人
	// 请先读完这段）：row 是 Qt 给的**视图行号**，落在这个 drop 容器（folder 的 model 子项，
	// 或 canvas 根）当前显示的行序里；moveNodes 的 destIndex 要的却是**store 子索引**，是
	// TreeNode::children 数组里的位置。这两套坐标只在"每个 store 子节点都恰好产生一行"时
	// 才重合。removeSceneWithUndo 故意把删除场景的僵尸节点留在 store 里等 Ctrl+Z，
	// SCENE_LIST_CHANGED 又不触发 resolveAndPrune 去清掉它，walkChildren 对活不下去的 uuid
	// 不产行——store 子数组因此能出现"没有对应行"的空洞，单纯数行号数出来的位置会比真实
	// store 索引偏小（复现：文件夹 [S1,S2,S3]，删 S2 留僵尸，视图只剩 [S1,S3] 两行，原先
	// "数行号"的算法会把"拖到底部"算成 store 索引 2——正好卡在 S3 前面，插入点相对 S3
	// 抵消掉，肉眼看不出任何移动）。
	// 改法：直接读目标行自身的 RolePath 末位当 store 索引，不再数行。目标行落在根部未归类
	// 尾区（不是 Folder 也不是 RolePlaced）时视同没有目标行，退回"最后一个已放置子项的
	// store 索引 + 1"（等价于 append）；容器里没有任何已放置子项时退回 0。moveNodes/
	// placeScene 内部仍会对这个值做既有的 clamp，这里不用重复夹一遍。
	auto containerRowCount = [&] {
		return container ? container->rowCount() : rowCount();
	};
	auto containerChild = [&](int i) {
		return container ? container->child(i) : item(i);
	};
	auto isPlacedRow = [](QStandardItem *c) {
		return c->data(RoleKind).toInt() == RowPlan::Folder || c->data(RolePlaced).toBool();
	};
	const int rowCnt = containerRowCount();
	int destIndex;
	if (row >= 0 && row < rowCnt && isPlacedRow(containerChild(row))) {
		const QVariantList pl = containerChild(row)->data(RolePath).toList();
		destIndex = pl.isEmpty() ? 0 : pl.last().toInt();
	} else {
		int lastPlaced = -1;
		for (int i = 0; i < rowCnt; ++i)
			if (isPlacedRow(containerChild(i)))
				lastPlaced = i;
		destIndex = (lastPlaced < 0) ? 0
					     : containerChild(lastPlaced)->data(RolePath).toList().last().toInt() + 1;
	}

	b->applyTreeOp(obs_module_text("SceneAnchor.Undo.Move"), [&]() {
		bool any = false;
		std::vector<NodePath> paths;
		std::vector<QString> unplaced;
		for (const auto &v : arr) {
			const QJsonObject o = v.toObject();
			if (o["canvas"].toString() != canvas)
				continue; // 跨 canvas 项静默丢弃
			if (o["placed"].toBool()) {
				NodePath p;
				for (const auto &pv : o["path"].toArray())
					p.push_back(pv.toInt());
				paths.push_back(std::move(p));
			} else {
				unplaced.push_back(o["uuid"].toString());
			}
		}
		// 已放置项与未归类项必须串接在同一个插入点上。moveNodes 会自行下调插入点（同父且在
		// 目标之前的项被移走），祖先吞并也会减少实际移动数——拿原始 destIndex + paths.size()
		// 推算会错位（反例：F=[A,B]，多选 F 内的 A 与未归类 S 拖到"B 之前"，S 会落到 B 之后，R-19）。
		int base = destIndex, moved = 0;
		if (!paths.empty()) {
			int at = destIndex, n = 0;
			if (b->store.moveNodes(canvas, paths, destFolder, destIndex, &at, &n)) {
				any = true;
				base = at;
				moved = n;
			}
		}
		int off = moved;
		for (const QString &u : unplaced)
			any |= b->store.placeScene(canvas, u, destFolder, base + off++);
		return any;
	});
	return false; // 不让 Qt 自己动行——applyTreeOp 已触发全量重建，二者打架（J-3）
}

TreeDock::TreeDock()
{
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(2);

	search_ = new QLineEdit(this);
	search_->setPlaceholderText(QString::fromUtf8(obs_module_text("SceneAnchor.Search")));
	search_->setClearButtonEnabled(true);
	lay->addWidget(search_);

	// 回车 = 切到第一个匹配的场景。没有这个的话搜索只兑现了一半：焦点在 QLineEdit 里时
	// 方向键是移动光标而不是移动选中项，用户必须回到鼠标去点结果。配上聚焦搜索框的全局
	// 热键（ObsBridge::registerHotkeys），键盘全程是「热键 → 打两个字 → 回车」。
	// 取"第一个匹配"用的是代理模型的可见顺序，即用户眼里从上往下第一条场景行；
	// 画布表头与文件夹跳过——它们不是可切换的东西。
	connect(search_, &QLineEdit::returnPressed, this, [this] {
		if (search_->text().isEmpty())
			return;
		auto *b = ObsBridge::get();
		// 走 QTreeView::indexBelow 而不是自己递归模型：indexBelow 给的是**视图可见顺序**，
		// 折叠起来的文件夹里的子项不会被走到。自己做深度优先会钻进折叠的文件夹，
		// 把节目切到一个屏幕上根本看不见的场景（过滤期间用户仍可手动折叠，
		// 而 expandWrite 在过滤时被抑制，所以视图确实折叠了、store 却不知道）。
		// 顺带省掉一个 std::function 和 <functional>。
		for (QModelIndex i = proxy_->index(0, 0, QModelIndex()); i.isValid(); i = view_->indexBelow(i)) {
			QStandardItem *it = itemAtSourceIndex(i);
			if (!it || it->data(RoleKind).toInt() != RowPlan::Scene)
				continue; // 文件夹不可切换
			// 用 transitionToScene 而非 switchToScene：右键「转场到此场景」与双击默认行为
			// 都走转场，回车若硬切，同一个 dock 里键盘和鼠标会给出不同的上画面效果。
			b->transitionToScene(it->data(RoleUuid).toString());
			return;
		}
	});
	// Esc 清空搜索。QLineEdit 自己不处理 Esc，而清空是这里唯一合理的期待。
	// WidgetShortcut 作用域：只在搜索框有焦点时生效，不抢 OBS 全局的 Esc。
	auto *escClear = new QShortcut(QKeySequence(Qt::Key_Escape), search_);
	escClear->setContext(Qt::WidgetShortcut);
	connect(escClear, &QShortcut::activated, search_, &QLineEdit::clear);
	// 全局热键把焦点送进来（热键回调已在 ObsBridge 侧排队到本线程）
	connect(ObsBridge::get(), &ObsBridge::focusSearchRequested, this, [this] {
		// 必须先让 dock 露出来。OBS 把我们的 widget 包在 OBSDock 里且注册时就 setVisible(false)
		// （OBSStudioAPI.cpp:342-350）；若用户把它关掉或与别的 dock 叠成标签页，
		// 对隐藏 widget 调 setFocus 只会记下"待获得焦点"，不会真的拿到键盘焦点——
		// 表现为"按了热键什么也没发生"，且无任何日志。
		if (auto *dw = qobject_cast<QDockWidget *>(parentWidget())) {
			dw->show();
			dw->raise();
		}
		search_->setFocus(Qt::ShortcutFocusReason);
		search_->selectAll(); // 直接打字即替换上次的关键词
	});

	// MRU 条包在横向滚动区里。不这么做的话，QHBoxLayout 会把所有 chip 的自然宽度之和
	// 当成自己的最小宽度，再原样传导给整个 dock（量具实测 915px，见 kMruChipMax 注释）。
	// 滚动区把这条链切断：dock 的最小宽度回落到树视图自己的 56px 量级。
	mruBar_ = new QWidget();
	mruLayout_ = new QHBoxLayout(mruBar_);
	mruLayout_->setContentsMargins(4, 0, 4, 0);
	mruLayout_->setSpacing(2);
	mruLayout_->addStretch();
	mruScroll_ = new QScrollArea(this);
	mruScroll_->setWidget(mruBar_);
	mruScroll_->setWidgetResizable(true);
	mruScroll_->setFrameShape(QFrame::NoFrame);
	mruScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	mruScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	mruScroll_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	lay->addWidget(mruScroll_);

	view_ = new AnchorTreeView(this);
	model_ = new AnchorModel(this);
	proxy_ = new QSortFilterProxyModel(this);
	proxy_->setSourceModel(model_);
	proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
	proxy_->setRecursiveFilteringEnabled(true);
	view_->setModel(proxy_);
	view_->setHeaderHidden(true);
	view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	// 缩进：默认 20 在窄 dock 里太宽（一级子项文字要缩进约 62px，直接吃掉场景名可读长度），
	// 但先前一路砍到 12 又走过头——配合当时去掉的场景图标，层级几乎看不出来，整列表读起来是平的。
	// 16 是两者之间：层级仍可辨，横向开销比默认小两成。
	view_->setIndentation(16);
	// 显式定图标尺寸，别让渲染尺寸随平台样式的默认值漂移——树的左边界是按它算的。
	view_->setIconSize(QSize(16, 16));
	// Task 7 刻意不设 EditTriggers/ItemIsEditable：当时没有 itemChanged 处理器，F2 打开的是 Qt
	// 内建的真实编辑器，输入的文字只进 QStandardItem、不落 store 也不改 OBS，下一次 rebuild 就
	// 静默蒸发——用户看到的是「改名成功了然后自己变回去」。本任务接上 itemChanged（见下方
	// connect）后一并打开，只用 F2 触发（不用单击/双击触发，双击已被行为设置项占用）。
	view_->setEditTriggers(QAbstractItemView::EditKeyPressed);
	view_->setContextMenuPolicy(Qt::CustomContextMenu);
	view_->setDragDropMode(QAbstractItemView::InternalMove);
	view_->setDefaultDropAction(Qt::MoveAction);
	view_->setDropIndicatorShown(true);
	lay->addWidget(view_, 1);

	// 空状态引导。没有任何文件夹时，这个 dock 看起来和 OBS 原生场景列表一模一样，
	// 没有任何东西提示"可以建文件夹"——真机上用户装了一整天都没建过一个。
	// 建了第一个文件夹后自动消失，不需要"不再提示"的开关。
	hint_ = new QLabel(QString::fromUtf8(obs_module_text("SceneAnchor.EmptyHint")), this);
	hint_->setWordWrap(true);
	hint_->setAlignment(Qt::AlignCenter);
	hint_->setForegroundRole(QPalette::PlaceholderText);
	hint_->setContentsMargins(10, 6, 10, 6);
	// 限高必须随 dock 高度走，不能写死。此前写死 64px，真机实测在 120 逻辑像素宽下
	// 中文提示需要 96px，于是被从句子中间裁断（屏幕上只剩「还没有文件夹 —— 用下方的
	// 文件夹按钮新」），后半句「建一个，再把场景拖进去」整段消失——裁一半的引导比
	// 不给引导更糟。而更长的译文（pt-BR 108 字符、fr-FR 112 字符）需要的还要多，
	// 任何写死的数都服务不了全部语言。
	// 改成占 dock 高度的三分之一：树永远保住三分之二，底部工具栏也绝不会被挤出去
	// （布局里只有 view_ 有 stretch，先被压缩的是它）。实际高度在 resizeEvent 里更新。
	updateHintCap();
	hint_->setVisible(false); // 由 rebuild() 按文件夹数量决定
	lay->addWidget(hint_);

	auto *btnRow = new QHBoxLayout();
	btnRow->setContentsMargins(4, 0, 4, 4);
	// 三个按钮此前是「文字 + / SVG 文件夹 / 文字 -」的混搭：两个字形与一个图标并排，
	// 且 '-' 是普通连字符，视觉重量远轻于 '+'。统一成同一套 16×16 实心图标，
	// 与两个行图标同源、同主题判定（见 rebuild()），不再有字形与图标混排。
	btnAddScene_ = new QToolButton(this);
	btnAddScene_->setToolTip(QString::fromUtf8(obs_module_text("SceneAnchor.AddScene")));
	btnAddFolder_ = new QToolButton(this);
	// fix round 2：图标改到 rebuild() 里跟两个行图标一起取（同一次主题判定），
	// 不在这里单独设一次——那次设定会在 THEME_CHANGED 之后再也不刷新，见 rebuild() 内注释。
	btnAddFolder_->setToolTip(QString::fromUtf8(obs_module_text("SceneAnchor.AddFolder")));
	btnRemove_ = new QToolButton(this);
	btnRemove_->setToolTip(QString::fromUtf8(obs_module_text("SceneAnchor.Remove")));
	btnRow->addWidget(btnAddScene_);
	btnRow->addWidget(btnAddFolder_);
	btnRow->addWidget(btnRemove_);
	btnRow->addStretch();
	lay->addLayout(btnRow);

	auto *b = ObsBridge::get();
	connect(b, &ObsBridge::needsRebuild, this, &TreeDock::rebuild);
	connect(b, &ObsBridge::sceneStateChanged, this, &TreeDock::onSceneStateChanged);
	connect(view_, &QTreeView::customContextMenuRequested, this, &TreeDock::onContextMenu);

	// 搜索实时过滤：接上 Task 7 建好但从未生效的 proxy_ 过滤器，第一次让视图行号与源模型行号分离。
	connect(search_, &QLineEdit::textChanged, this, [this](const QString &t) {
		proxy_->setFilterFixedString(t);
		if (!t.isEmpty()) {
			view_->expandAll(); // 命中项的父链全开，肉眼可见
			// 过滤中禁拖。风险不在算术（placedCount/destIndex 全程走 QStandardItem 的源模型坐标，
			// 不受代理影响），而在语义：递归过滤会让文件夹可见而部分子项隐藏，「拖到这两个可见行
			// 之间」映射回源位置后可能不是用户看到的位置。NoDragDrop 按 Qt 文档契约不支持拖放，
			// 三个 DnD 覆写因而不会被调用——具体由哪一层拦下未经源码确认（obs-deps 的 Qt 只有
			// 头文件与库，无 .cpp），但对外可观察的保证一致。
			view_->setDragDropMode(QAbstractItemView::NoDragDrop);
		} else {
			view_->setDragDropMode(QAbstractItemView::InternalMove);
			rebuild(); // 恢复 store 记录的展开态——过滤期间 expandWrite 已抑制回写，store 未被
				   // expandAll() 或过滤中的手动折叠污染，这里重建即是精确回到过滤前的样子
		}
	});

	// F2/菜单改名提交 → 落到 store（Folder）或 OBS（Scene）。rebuilding_ 在这里不是防重入的
	// 惯例性防线，而是承重的：QStandardItemModel 把任何一次 setData 的 dataChanged 都转发成
	// itemChanged(QStandardItem*)，不按 role 过滤——setFont（FontRole）、setIcon、着色
	// 都会触发它，不只是文字改名。onSceneStateChanged() 的字体高亮就会经这条路径落到下面，
	// 若不挡住会被 Scene 分支当成一次真实改名提交给 OBS（Critical，最终整支审查发现，
	// 该函数内有说明）。Scene 分支另外比对 OBS 当前实时名字做第二道防线——两者互补，
	// 不是同一道防线的重复：rebuilding_ 防的是"这次 setData 发生在我们自己已知的重建/
	// 高亮过程里"，名字比对防的是"就算 rebuilding_ 意外没罩住，文本也没真的变"。
	connect(model_, &QStandardItemModel::itemChanged, this, [this](QStandardItem *it) {
		if (rebuilding_)
			return;
		auto *bb = ObsBridge::get();
		const int kind = it->data(RoleKind).toInt();
		if (kind == RowPlan::Folder) {
			const QString cv = it->data(RoleCanvas).toString();
			const NodePath p = pathOfItem(it);
			const QString name = it->text().trimmed();
			// 空名：renameFolder 会拒绝（返回 false），applyTreeOp 因而不会重建——item 显示会卡在
			// 用户刚提交的空字符串上，直到下次不相关的 rebuild 才刷回原名，等于又一次"改名成功然后
			// 自己变回去"的静默蒸发（本任务要根治的那类症状）。跟 renameScene 对空名的处理对齐，
			// 强制立即刷回。
			if (name.isEmpty()) {
				bb->needsRebuild();
				return;
			}
			// fix round 1 Important：提交了跟当前存的名字完全一样的名字（原样回车，或改了又改回来）——
			// renameFolder 会返回 true 但 before==after，applyTreeOp 提前 return 不会 emit
			// needsRebuild()，若还是照常设了 pendingRename*_，这几个成员会摆在原处直到某次不相关的
			// rebuild 才被消费。今天推不出真的选错（Task 9 的位置+名字双重校验兜底），但这份"安全"
			// 靠的是一条没写下来的隐含前提，不如在源头短路掉——跟空名短路同一位置，此处不用刷新
			// （显示本就没错，不像空名那种需要强制刷回）。
			if (const TreeNode *cur = bb->store.nodeAt(cv, p); cur && cur->name == name)
				return;
			// K-2②（J-7 改名缺口）：记下这次改名的目标，供本次改名触发的 rebuild() 做选中恢复时
			// 使用，不依赖 it->text() 在嵌套/后续 rebuild 里仍然可靠——见头文件里对这几个成员的注释。
			pendingRenameCanvas_ = cv;
			pendingRenamePath_ = p;
			pendingRenameName_ = name;
			hasPendingRename_ = true;
			bb->applyTreeOp(obs_module_text("SceneAnchor.Undo.RenameFolder"),
					[&] { return bb->store.renameFolder(cv, p, name); });
		} else if (kind == RowPlan::Scene) {
			// 第二道防线（见上方注释）：文本跟 OBS 里的实时名字比对，一样就短路，不当改名提交。
			// 这样即便本次 itemChanged 不是来自真的改名（字体高亮之类），也不会误发一次
			// obs_source_set_name——不依赖 rebuilding_ 这一道防线独自扛住。
			const QString uuid = it->data(RoleUuid).toString();
			const QString text = it->text();
			obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
			const QString live = s ? QString::fromUtf8(obs_source_get_name(s)) : QString();
			if (s)
				obs_source_release(s);
			if (live == text)
				return;
			bb->renameScene(uuid, text);
		}
	});

	// 展开态回写 store（用户手动展开/折叠；重建与过滤中忽略）
	auto expandWrite = [this](const QModelIndex &pi, bool exp) {
		if (rebuilding_ || !search_->text().isEmpty())
			return;
		QStandardItem *it = itemAtSourceIndex(pi);
		if (!it)
			return;
		const QString cv = it->data(RoleCanvas).toString();
		const int kind = it->data(RoleKind).toInt();
		if (kind != RowPlan::Folder)
			return;
		const NodePath p = pathOfItem(it);
		ObsBridge::get()->silentTreeOp([cv, p, exp] { ObsBridge::get()->store.setExpanded(cv, p, exp); });
	};
	connect(view_, &QTreeView::expanded, this, [expandWrite](const QModelIndex &i) { expandWrite(i, true); });
	connect(view_, &QTreeView::collapsed, this, [expandWrite](const QModelIndex &i) { expandWrite(i, false); });

	// 单击/键盘选中 → 切换（原生行为：非 studio 切 program，studio 设预览）。
	// 副画布场景的拒绝已下沉进 ObsBridge::switchToScene 内部（R-15），这里直接调用即可。
	//
	// 可关：这里是 currentChanged，方向键路过的**每一行**都会切一次直播画面。原生场景列表
	// 也是这个行为，但那个列表只用来切换；本 dock 还要拖拽/改名/上色，导航与操作共用一个
	// 控件——把末尾的场景拖进文件夹，方向键走 8 行就是 8 次直播切换。默认保持开（匹配原生，
	// 且把原生场景 dock 关掉、拿本 dock 当场景列表的人需要它），整理时可临时关掉。
	// 关掉后仍可用双击（模式为「转场」时）或右键「切换」明确切换，不会失去切换手段。
	connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this,
		[this](const QModelIndex &cur, const QModelIndex &) {
			if (rebuilding_ || !cur.isValid())
				return;
			if (!ObsBridge::get()->option(kOptSelectSwitches.key, kOptSelectSwitches.def))
				return;
			QStandardItem *it = itemAtSourceIndex(cur);
			if (it && it->data(RoleKind).toInt() == RowPlan::Scene)
				ObsBridge::get()->switchToScene(it->data(RoleUuid).toString());
		});

	connect(view_, &QTreeView::doubleClicked, this, [this](const QModelIndex &pi) {
		QStandardItem *it = itemAtSourceIndex(pi);
		if (!it || it->data(RoleKind).toInt() != RowPlan::Scene)
			return;
		auto *b = ObsBridge::get();
		const QString mode = b->doubleClickMode();
		if (mode == QLatin1String("transition"))
			b->transitionToScene(it->data(RoleUuid).toString());
		else if (mode == QLatin1String("rename"))
			view_->edit(pi); // Task 10 加 ItemIsEditable 前，非编辑态下这是无操作
	});

	connect(btnAddFolder_, &QToolButton::clicked, this, [this] {
		auto *bb = ObsBridge::get();
		// 落位：选中 folder → 其内末尾；选中 scene(placed) → 同级其后；否则根末尾
		QString canvas = bb->liveCanvases().empty() ? QString() : bb->liveCanvases().front().uuid;
		NodePath parent;
		int index = INT_MAX;
		if (view_->currentIndex().isValid()) {
			QStandardItem *it = itemAtSourceIndex(view_->currentIndex());
			canvas = it->data(RoleCanvas).toString();
			const NodePath p = pathOfItem(it);
			if (it->data(RoleKind).toInt() == RowPlan::Folder) {
				parent = p;
			} else if (it->data(RolePlaced).toBool() && !p.empty()) {
				parent.assign(p.begin(), p.end() - 1);
				index = p.back() + 1;
			}
		}
		if (canvas.isEmpty())
			return;
		bb->applyTreeOp(obs_module_text("SceneAnchor.Undo.AddFolder"), [&] {
			return bb->store.insertFolder(canvas, parent, index,
						      QString::fromUtf8(obs_module_text("SceneAnchor.NewFolder")));
		});
	});

	connect(btnAddScene_, &QToolButton::clicked, this, [this] {
		auto *bb = ObsBridge::get();
		const auto live = bb->liveCanvases();
		if (live.empty())
			return;
		QString canvas = live.front().uuid;
		NodePath folder;
		if (view_->currentIndex().isValid()) {
			QStandardItem *it = itemAtSourceIndex(view_->currentIndex());
			if (it->data(RoleKind).toInt() == RowPlan::Folder) {
				canvas = it->data(RoleCanvas).toString();
				folder = pathOfItem(it);
			}
		}
		if (canvas != live.front().uuid)
			return; // 副画布不建场景（无切换语义）
		bb->createSceneInFolder(canvas, folder);
	});

	connect(btnRemove_, &QToolButton::clicked, this, [this] {
		if (!view_->currentIndex().isValid())
			return;
		QStandardItem *it = itemAtSourceIndex(view_->currentIndex());
		auto *bb = ObsBridge::get();
		const int kind = it->data(RoleKind).toInt();
		if (kind == RowPlan::Folder) {
			// 文件夹删除永不删场景：场景回未归类（spec delta 3）
			const QString cv = it->data(RoleCanvas).toString();
			const NodePath p = pathOfItem(it);
			bb->applyTreeOp(obs_module_text("SceneAnchor.Undo.RemoveFolder"),
					[&] { return bb->store.removeNode(cv, p); });
		} else if (kind == RowPlan::Scene) {
			// Task 9 留空处（J-6），本任务补上：与右键菜单的删除走同一条确认+undo 路径。
			if (QMessageBox::question(
				    this, QString::fromUtf8(obs_module_text("SceneAnchor.Menu.RemoveScene")),
				    QString::fromUtf8(obs_module_text("SceneAnchor.ConfirmRemove")).arg(it->text())) ==
			    QMessageBox::Yes)
				bb->removeSceneWithUndo(it->data(RoleUuid).toString());
		}
	});

	obs_log(LOG_INFO, "dock constructed");
}

QStandardItem *TreeDock::itemAtSourceIndex(const QModelIndex &proxyIdx) const
{
	return model_->itemFromIndex(proxy_->mapToSource(proxyIdx));
}

NodePath TreeDock::pathOfItem(const QStandardItem *it) const
{
	NodePath p;
	for (const QVariant &v : it->data(RolePath).toList())
		p.push_back(v.toInt());
	return p;
}

QModelIndex TreeDock::findSceneIndex(const QString &uuid) const
{
	for (QStandardItem *it : model_->findItems(QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive))
		if (it->data(RoleKind).toInt() == RowPlan::Scene && it->data(RoleUuid).toString() == uuid)
			return proxy_->mapFromSource(model_->indexFromItem(it));
	return {};
}

QModelIndex TreeDock::findFolderIndex(const QString &canvas, const NodePath &path) const
{
	for (QStandardItem *it : model_->findItems(QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive))
		if (it->data(RoleKind).toInt() == RowPlan::Folder && it->data(RoleCanvas).toString() == canvas &&
		    pathOfItem(it) == path)
			return proxy_->mapFromSource(model_->indexFromItem(it));
	return {};
}

void TreeDock::rebuild()
{
	if (rebuilding_)
		return;
	rebuilding_ = true;

	// Task 12 fix round 1：图标按主题分色（见 anchorFolderIcon/anchorSceneIcon），
	// THEME_CHANGED 触发的这次 rebuild 必须真的换色——static 缓存会把第一次主题下的
	// 图标焊死，之后再怎么切主题都读不到新文件。改普通局部变量，每次 rebuild 现取，
	// 相比重建整个 model 这点 I/O 可以忽略。
	// fix round 3：深浅色的判定改问 view_ 的实际调色板而非 obs_frontend_is_theme_dark()
	// （原因见 anchorIcon 上方注释），行图标与工具栏按钮图标共用这同一次判定结果。
	const QColor viewBg = view_->palette().color(QPalette::Base);
	const bool darkIcons = viewBg.lightness() < 128;
	const QIcon folderIcon = anchorFolderIcon(darkIcons);
	const QIcon sceneIcon = anchorSceneIcon(darkIcons);
	// 场景行图标默认**开**。曾经改成默认关，理由是"文件夹和场景都有图标则图标不说明任何事，
	// 关掉后有图标=文件夹才是真信号"，且 OBS 原生场景列表确实零图标。那个论证本身没错，
	// 但它输给排版：原生列表是**平铺**的，只有一种节点，无需类型区分；本插件的树在同一层
	// 同时放文件夹与场景，文件夹有图标而场景没有，文字左边缘就是参差的；再加上整行只剩
	// 文字，行高 22 逻辑像素配纯文字，密度与留白严重失衡，真机上看就是"没做完"。
	// 保留选项：想要原生那种纯文字观感的人一个右键即可关掉（本机用户在上一代插件里就是关的）。
	const bool icons = ObsBridge::get()->option(kOptIcons.key, kOptIcons.def);
	// fix round 2：工具栏的新建文件夹按钮跟树行共用同一次主题判定，不再在构造函数里
	// 单独设一次——那次是一锤子买卖，主题切换后再也不会更新，而这里每次 rebuild 都过。
	// 另两个按钮同理：构造函数里设一次会在切主题后留下错色图标。
	btnAddFolder_->setIcon(folderIcon);
	btnAddScene_->setIcon(anchorPlusIcon(darkIcons));
	btnRemove_->setIcon(anchorMinusIcon(darkIcons));

	// 保存视图状态。Folder 行的 RoleUuid 恒为空（J-7）：只存 selUuid 会让选中文件夹时
	// selUuid 取到空串，下面的恢复逻辑会静默回落到"当前场景"——本任务首次能建文件夹，此缺陷随即可达。
	// 因此 Folder 额外记 (canvas, path, name) 作为恢复键；Scene 仍按 uuid（跨重建路径不变，uuid 才稳）。
	// path 是位置匹配不是身份匹配：拖拽（本任务旗舰功能）会让别的文件夹滑进原文件夹腾出的
	// 那个 (canvas, path)，仅凭位置匹配会静默选中"别人"。name 不是唯一 key，但配合位置足够
	// 强——恢复时位置+名字任一对不上就清空选中，只退化成"没选中"而不是"选错"（Important 修复）。
	QString selUuid, selFolderCanvas, selFolderName;
	NodePath selFolderPath;
	bool selIsFolder = false;
	if (view_->currentIndex().isValid())
		if (QStandardItem *it = itemAtSourceIndex(view_->currentIndex())) {
			selUuid = it->data(RoleUuid).toString();
			if (it->data(RoleKind).toInt() == RowPlan::Folder) {
				selIsFolder = true;
				selFolderCanvas = it->data(RoleCanvas).toString();
				selFolderPath = pathOfItem(it);
				selFolderName = it->text();
			}
		}
	// K-2②（J-7 改名缺口）：若这次 rebuild 正是由"刚改名一个 Folder"触发的，用改名时记下的新名字
	// 覆盖上面捕获到的 selFolderName——不依赖 it->text() 在这次 rebuild 里必然已经是新名字。
	// path 不变（renameFolder 只改名不挪位置），所以只需要覆盖名字这一项。一次性消费，不管这次
	// 是否命中都清空，避免误用到跟这次改名无关的后续 rebuild。
	if (selIsFolder && hasPendingRename_ && selFolderCanvas == pendingRenameCanvas_ &&
	    selFolderPath == pendingRenamePath_)
		selFolderName = pendingRenameName_;
	hasPendingRename_ = false;
	const int scroll = view_->verticalScrollBar()->value();

	auto *b = ObsBridge::get();
	const auto live = b->liveCanvases();
	const auto plan = planProjection(b->store, live);

	model_->removeRows(0, model_->rowCount());
	std::vector<QStandardItem *> parents{model_->invisibleRootItem()};
	std::vector<std::pair<QStandardItem *, bool>> expandStates;
	int folderRows = 0, sceneRows = 0, unfiledRows = 0;

	for (const auto &r : plan) {
		auto *item = new QStandardItem(r.name);
		item->setData(int(r.kind), RoleKind);
		item->setData(r.uuid, RoleUuid);
		QVariantList pl;
		for (int i : r.path)
			pl << i;
		item->setData(pl, RolePath);
		item->setData(r.canvas, RoleCanvas);
		item->setData(r.placed, RolePlaced);
		if (!r.color.isEmpty())
			item->setData(r.color, RoleColor); // 右键菜单读它来标出当前选中的色
		Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
		if (r.kind == RowPlan::Folder)
			f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable;
		else if (r.kind == RowPlan::Scene)
			f |= Qt::ItemIsDragEnabled | Qt::ItemIsEditable;
		else
			f = Qt::ItemIsEnabled | Qt::ItemIsDropEnabled; // header：不可选，不可编辑
		item->setFlags(f);
		// 画布表头此前与场景行同字重同色，看着像可点的树项，实际不可选。用调色板的
		// 颜色标签 = 给这一行已有的图标染色（见 tintedIcon 上方的三轮取舍记录）。
		// 上色前先按实际背景调对比度：存的是原色，画的是可读的那一版。
		const QColor raw(r.color);
		const QColor tag = raw.isValid() ? contrastAdjusted(raw, viewBg) : QColor();
		// 图标开关必须**同时**管文件夹与场景，不能只管场景。QTreeView 的缩进是
		// (层级 + 根装饰) * indentation，补不回图标那一栏的宽度：只关场景图标的话，
		// 顶层场景名与顶层文件夹名会差出整整一个图标宽（真机实测 41 / 74），而文件夹
		// **内**的场景名反倒比其父文件夹名更靠左（66 / 74），层级在视觉上是反的。
		// 也试过给场景行塞等宽透明占位来对齐——对齐是对齐了，但关掉图标仍白占 24
		// 物理像素，既没图标也没省出地方，在 120 逻辑像素宽的 dock 上尤其亏。
		// 两边一起关最干净：同层共用一条左边界、子项必然比父项靠右、宽度一分不浪费。
		// 代价是空文件夹失去图标标识。拖放时不受影响：canDropMimeData 对场景返回 false，
		// 只有文件夹会出现「落入」指示框，所以拖的时候仍分得清，未做额外补偿。
		if (icons) {
			const QIcon &base = r.kind == RowPlan::Folder ? folderIcon : sceneIcon;
			item->setIcon(tag.isValid() ? tintedIcon(base, tag) : base);
		} else if (tag.isValid()) {
			item->setForeground(tag); // 没有图标可染，颜色标签落到文字上
		}
		parents[r.depth]->appendRow(item);
		parents.resize(r.depth + 1);
		parents.push_back(item);
		if (r.kind != RowPlan::Scene)
			expandStates.push_back({item, r.expanded});
		if (r.kind == RowPlan::Folder)
			++folderRows;
		else if (r.kind == RowPlan::Scene) {
			++sceneRows;
			if (!r.placed)
				++unfiledRows;
		}
	}

	obs_log(LOG_INFO, "rebuild: %d rows (%d folders, %d scenes, %d unfiled) across %zu canvas(es), mru=%d",
		(int)plan.size(), folderRows, sceneRows, unfiledRows, live.size(), (int)b->store.mru().size());

	// 文件夹有没有内容：构建完再回填。drawBranches 要用它来决定箭头的浓淡，而在绘制路径里
	// 调 QSortFilterProxyModel::rowCount() 会创建代理的内部映射——那是在 paint 期间改动模型
	// 状态，Qt 明确不建议。这里在构建期算好存进 role，绘制时只读。
	for (const auto &[it, exp] : expandStates)
		it->setData(it->rowCount() > 0, RoleHasKids);
	for (const auto &[it, exp] : expandStates)
		view_->setExpanded(proxy_->mapFromSource(model_->indexFromItem(it)), exp);

	// 过滤生效期间，展开态由过滤决定而非 store。外部事件（热键改名、脚本增删场景、切主题）
	// 触发的 rebuild 若不重新展开，命中项的父链会折回 store 记录的状态 —— 用户眼中就是
	// 「刚搜到的东西自己消失了」，要等下一次按键才回来。直播场景下热键与脚本是常驻的。
	if (!search_->text().isEmpty())
		view_->expandAll();

	// 恢复选中：原选中是 folder → 按 (canvas, path) 找，且要求 name 也对得上，两者任一对不上
	// 就不选（宁可"没选中"，不可"选错"——拖拽会让别的文件夹滑进原文件夹腾出的位置，见 J-7）；
	// 原选中是 scene → 按 uuid 找；都没有（原本无选中）→ 落到当前场景。
	// selIsFolder 时 selUuid 恒空，不会误落到当前场景。
	const QString target = (!selUuid.isEmpty() || selIsFolder) ? selUuid : b->currentSceneUuid();
	if (!target.isEmpty() || selIsFolder) {
		for (QStandardItem *it :
		     model_->findItems(QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive)) {
			const int kind = it->data(RoleKind).toInt();
			const bool sceneMatch = !target.isEmpty() && kind == RowPlan::Scene &&
						it->data(RoleUuid).toString() == target;
			const bool folderMatch = selIsFolder && kind == RowPlan::Folder &&
						 it->data(RoleCanvas).toString() == selFolderCanvas &&
						 pathOfItem(it) == selFolderPath && it->text() == selFolderName;
			if (sceneMatch || folderMatch) {
				view_->setCurrentIndex(proxy_->mapFromSource(model_->indexFromItem(it)));
				break;
			}
		}
	}
	// 先决定提示的显隐再恢复滚动位置：反过来的话，显隐引发的重排会改变滚动条量程，
	// 刚设好的值被夹掉，用户眼里就是"建完文件夹视图跳了一下"。
	hintWanted_ = folderRows == 0;
	updateHintCap(); // 由它统一决定可见性（宽/高不够时整段隐藏）
	// 根层的展开箭头栏，只在**顶层确实有一个能展开的文件夹**时才占位。
	//
	// QTreeView 的缩进是 (层级 + rootIsDecorated) * indentation，是按**层**生效的：
	// 一旦打开，同层的每一行都要让出一格，包括不需要箭头的场景行。而 Qt 又只给
	// hasChildren 为真的行画箭头。两条规则叠在一起就有一个白付钱的状态：
	// 新建一个**空**文件夹——Qt 不会给它画箭头，整棵树却已经右移了一格（实测 24 物理
	// 像素 @150% DPI），为一个根本没出现的箭头买单。用户一建文件夹就看到全体右移，
	// 却找不到多出来的是什么东西。
	//
	// 曾经把判据收紧成"顶层有没有文件夹真的带着子项"，因为 Qt 不给空文件夹画箭头，
	// 那一格是白留的。现在箭头由 AnchorTreeView::drawBranches 自己画、空文件夹照样有，
	// 这一格不再白留，判据回到"顶层有没有文件夹"——也更可预期：有文件夹就有文件夹列。
	bool rootFolder = false;
	for (const auto &r : plan)
		if (r.kind == RowPlan::Folder && r.depth == 0) {
			rootFolder = true;
			break;
		}
	view_->setRootIsDecorated(rootFolder);
	view_->verticalScrollBar()->setValue(scroll);
	rebuilding_ = false;
	onSceneStateChanged();
}

void TreeDock::onSceneStateChanged()
{
	// 字体高亮是纯显示变更，但 setFont → setData(FontRole) → dataChanged → itemChanged，
	// 而 itemChanged 处理器不按 role 过滤，会把它当成改名提交（Critical，最终整支审查发现）。
	// rebuilding_ 挡不住：rebuild() 在调用本函数的前一行就已清除它，SCENE_CHANGED 路径更从未设过。
	const bool wasRebuilding = rebuilding_;
	rebuilding_ = true;
	auto *b = ObsBridge::get();
	const QString prog = b->currentSceneUuid();
	const QString prev = b->currentPreviewUuid();
	for (QStandardItem *it : model_->findItems(QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive)) {
		if (it->data(RoleKind).toInt() != RowPlan::Scene)
			continue;
		QFont f = it->font();
		const QString u = it->data(RoleUuid).toString();
		f.setBold(u == prog);
		f.setItalic(!prev.isEmpty() && u == prev);
		it->setFont(f);
	}
	rebuilding_ = wasRebuilding; // 恢复而非硬置 false：万一未来某处从 rebuild 内部调用本函数，不破坏外层的保护
	refreshMru();
}

// 提示文字的限高：见构造里 updateHintCap() 调用处的说明。
void TreeDock::updateHintCap()
{
	const int cap = qMax(48, height() / 3);
	hint_->setMaximumHeight(cap);
	// 放不下就整段不显示。截一半的引导读起来是"这里坏了"——真机上量到过
	// 「还没有文件夹 —— 用下方的 文件夹按钮新」后面整句消失。宁可不给引导：
	// 它只是新手上手的提示，不是功能，缺席无害，残缺有害。
	// 用 heightForWidth 按当前可用宽度算真实换行高度，比预估行数可靠；
	// 减 20 是 hint_ 自己的左右 contentsMargins（10+10）。
	const bool fits = hint_->heightForWidth(qMax(1, width() - 20)) <= cap;
	hint_->setVisible(hintWanted_ && fits);
}

void TreeDock::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	updateHintCap();
	if (qAbs(width() - mruWidth_) > 8)
		refreshMru();
}

void TreeDock::refreshMru()
{
	while (mruLayout_->count() > 1) { // 尾部 stretch 保留
		QLayoutItem *li = mruLayout_->takeAt(0);
		if (li->widget())
			li->widget()->deleteLater();
		delete li;
	}
	mruWidth_ = width();
	auto *b = ObsBridge::get();
	// 关掉时直接隐藏并返回：上面的清空循环已经把 chip 摘干净，这里不再枚举 liveCanvases，
	// 省掉一次对 OBS 的场景遍历（本函数每次场景切换都会跑）。
	if (!b->option(kOptShowMru.key, kOptShowMru.def)) {
		mruScroll_->setVisible(false);
		return;
	}
	const auto live = b->liveCanvases();
	if (live.empty())
		return;
	std::map<QString, QString> names;
	for (const auto &s : live.front().scenes)
		names[s.uuid] = s.name; // MRU 只做主画布
	const QStringList mru = b->store.mru();

	// chip 宽度按当前可用宽度均分，而不是恒用上限。恒用上限时五个 chip 在真机 449 逻辑像素
	// 宽的 dock 里必然溢出，滚动区就常驻一条 12px 高的横向滚动条——MRU 区域从 34px 涨到 46px，
	// 在一个 dock 里这点纵向空间不便宜。均分后常见宽度下不再出现滚动条；窄到连下限都放不下时
	// 才让它出现，那种宽度下本来也塞不进五个有名字的按钮。
	int live_ = 0;
	for (const QString &u : mru)
		if (names.count(u))
			++live_;
	const int avail = mruScroll_->viewport()->width() - 8;
	// 先算这个宽度下按可读下限最多能放几个，再让它们均分——顺序不能反。先均分再看放不放得下
	// 的话，窄 dock 上五个 chip 会各自缩到 40px 出头（真机 217 逻辑像素宽时实测缩到只剩
	// "自习室…" "YY开…" 这种四字残段）并顶出一条横向滚动条：既读不出是哪个场景，又白吃
	// 12px 纵向。少显示几个、每个都读得全，比五个都读不全有用。
	const int maxFit = std::max(1, (avail + 2) / (kMruChipMin + 2));
	const int show = std::min(live_, maxFit);
	int cap = kMruChipMax;
	if (show > 0)
		cap = std::clamp((avail - 2 * (show - 1)) / show, kMruChipMin, kMruChipMax);

	int inserted = 0;
	int chipH = 0;
	for (const QString &u : mru) {
		auto it = names.find(u);
		if (it == names.end())
			continue;
		if (inserted >= show)
			break; // 放不下的不画，而不是画出来再挤扁
		auto *chip = new QToolButton(mruBar_);
		chip->setAutoRaise(true);
		// 场景名可以很长（真机上单个 chip 自然宽度达 231px）。封顶 + 省略，全名进 tooltip。
		// 省略位置取中间而非结尾：OBS 里的场景名普遍按类别加前缀（真机上就有
		// 「YY开播-去背景|加底图」与「YY开播-小头像」），从尾部截会把两个 chip 都截成
		// 「YY开播-…」——两个按钮长得一模一样，等于没有。从中间截保住首尾，
		// 「YY开…加底图」与「YY开…小头像」仍可区分。
		chip->setMaximumWidth(cap);
		chip->setText(QFontMetrics(chip->font()).elidedText(it->second, Qt::ElideMiddle, cap - 16));
		chip->setToolTip(it->second);
		connect(chip, &QToolButton::clicked, this, [u] { ObsBridge::get()->switchToScene(u); });
		chipH = std::max(chipH, chip->sizeHint().height());
		mruLayout_->insertWidget(inserted++, chip);
	}
	mruScroll_->setVisible(inserted > 0);
	if (inserted > 0) {
		// 高度取 chip 自己的 sizeHint，不取 mruBar_->sizeHint()：后者在此刻布局尚未 activate，
		// 真机实测返回的是空布局的最小高度（12 逻辑像素），滚动区因此被设成 18 物理像素高，
		// 把 40 物理像素高的 chip 裁掉大半、并被下方树视图盖住（UIA 量得 scroll=18 / chip=40）。
		// chip 是本函数刚创建的，它的 sizeHint 与布局激活时机无关。
		// show 只保证"按下限放得下"；dock 窄到连一个下限宽 chip 都放不下时滚动条仍会出现，
		// 那时要额外留出它的高度，否则又被裁掉。
		const int need = inserted * cap + 2 * (inserted - 1) + 8;
		const int sb = need > avail ? mruScroll_->horizontalScrollBar()->sizeHint().height() : 0;
		mruScroll_->setFixedHeight(chipH + sb);
	}
	// 两个原因都会让 chip 少于 mru 条数，分开报：陈旧条目是数据问题，宽度不足是布局问题，
	// 混成一句会让下一个看日志的人误判。
	const int stale = (int)mru.size() - live_;
	const int clipped = live_ - inserted;
	if (stale > 0 || clipped > 0)
		obs_log(LOG_INFO, "refreshMru: %d/%d shown (%d stale, %d dropped for width, cap=%d, avail=%d)",
			inserted, (int)mru.size(), stale, clipped, cap, avail);
}

// 右键菜单全集：场景 / 文件夹 / 空白区三分支。K-5：不加「属性」「交互」——原生场景右键菜单本就没有，
// 场景无属性对话框。树里只有主画布场景（副画布不进树，理由见 ObsBridge::liveCanvases），
// 因而不再有"这一项对某些场景不可用"的分支。K-4：转场覆盖/多画面显隐读写的是
// obs_source_get_private_settings 的前端约定 key，灰区 API，spec 已声明，照做不改进。
void TreeDock::onContextMenu(const QPoint &pos)
{
	auto *b = ObsBridge::get();
	QMenu menu(this);
	const QModelIndex pi = view_->indexAt(pos);
	QStandardItem *it = pi.isValid() ? itemAtSourceIndex(pi) : nullptr;
	const int kind = it ? it->data(RoleKind).toInt() : -1;

	auto addColorMenu = [&](QStandardItem *target) {
		QMenu *cm = menu.addMenu(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Color")));
		// fix round 1 Important：target 只在这里（菜单构建时，同步）解引用一次，取出纯值。
		// apply 的点击回调不再持有 target 本身——exec() 期间若树被外部事件重建，target 会悬空。
		const QString cv = target->data(RoleCanvas).toString();
		const NodePath basePath = pathOfItem(target);
		const QString uuid = target->data(RoleUuid).toString();
		const bool isUnplacedScene = target->data(RoleKind).toInt() == RowPlan::Scene &&
					     !target->data(RolePlaced).toBool();
		auto apply = [this, b, cv, basePath, uuid, isUnplacedScene](const QString &color) {
			// 未归类场景着色 → 先落位根末尾（进 store）
			NodePath p = basePath;
			b->applyTreeOp(obs_module_text("SceneAnchor.Undo.Color"), [&] {
				if (isUnplacedScene) {
					if (!b->store.placeScene(cv, uuid, {}, INT_MAX))
						return false;
					auto found = b->store.findScene(cv, uuid);
					if (!found)
						return false;
					p = *found;
				}
				return b->store.setColor(cv, p, color);
			});
		};
		// 菜单项此前直接显示十六进制码（"#d13438"），对用户毫无意义；改用颜色名。
		// 色块也过一遍 contrastAdjusted，让菜单里看到的就是行上会画出来的那个色。
		const QColor menuBg = view_->palette().color(QPalette::Base);
		const QColor menuFg = view_->palette().color(QPalette::Text);
		const QString cur = target->data(RoleColor).toString();
		for (const Preset &pc : kColors) {
			QAction *a = cm->addAction(
				QString::fromUtf8(obs_module_text(QByteArray("SceneAnchor.Color.") + pc.key)));
			const bool isCur = cur.compare(QString::fromUtf8(pc.hex), Qt::CaseInsensitive) == 0;
			a->setIcon(QIcon(colorSwatch(QColor(QString::fromUtf8(pc.hex)), menuBg, menuFg, isCur)));
			const QByteArray hex(pc.hex);
			connect(a, &QAction::triggered, this, [apply, hex] { apply(QString::fromUtf8(hex)); });
		}
		cm->addSeparator();
		// 自定义配色：store 里的 color 本来就是任意字符串、渲染走 QColor(QString)，
		// 所以这条只是补一个入口，没有新的存储或渲染路径。选出来的颜色同样经
		// contrastAdjusted 保证可读，用户不会因为挑了个深蓝就得到一个看不见的标签。
		QAction *custom = cm->addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.CustomColor")));
		if (QColor(cur).isValid() && !presetHas(cur))
			custom->setIcon(QIcon(colorSwatch(QColor(cur), menuBg, menuFg, true)));
		connect(custom, &QAction::triggered, this, [this, apply, cur] {
			const QColor init = QColor(cur).isValid() ? QColor(cur) : QColor(Qt::white);
			const QColor picked = QColorDialog::getColor(
				init, this, QString::fromUtf8(obs_module_text("SceneAnchor.Menu.CustomColor")));
			if (picked.isValid())
				apply(picked.name(QColor::HexRgb));
		});
		QAction *clear = cm->addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.ClearColor")));
		connect(clear, &QAction::triggered, this, [apply] { apply(QString()); });
	};

	if (kind == RowPlan::Scene) {
		const QString uuid = it->data(RoleUuid).toString();

		QAction *sw = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Switch")));
		connect(sw, &QAction::triggered, this, [b, uuid] { b->transitionToScene(uuid); });
		menu.addSeparator();
		// fix round 1 Important：改名按 uuid 在点击时重新定位行（见 findSceneIndex 注释），
		// 不捕获 pi——exec() 期间树可能已被外部事件重建，pi 会失效。
		QAction *ren = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Rename")));
		connect(ren, &QAction::triggered, this, [this, uuid] {
			const QModelIndex idx = findSceneIndex(uuid);
			if (idx.isValid())
				view_->edit(idx);
		});
		QAction *dup = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Duplicate")));
		connect(dup, &QAction::triggered, this, [b, uuid] { b->duplicateScene(uuid); });
		QAction *rm = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.RemoveScene")));
		// 确认框文案用的场景名在菜单构建时取值捕获（sceneName），不捕获 it 本身——同上理由。
		const QString sceneName = it->text();
		connect(rm, &QAction::triggered, this, [this, b, uuid, sceneName] {
			if (QMessageBox::question(
				    this, QString::fromUtf8(obs_module_text("SceneAnchor.Menu.RemoveScene")),
				    QString::fromUtf8(obs_module_text("SceneAnchor.ConfirmRemove")).arg(sceneName)) ==
			    QMessageBox::Yes)
				b->removeSceneWithUndo(uuid);
		});
		menu.addSeparator();

		QAction *cf = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.CopyFilters")));
		connect(cf, &QAction::triggered, this, [b, uuid] { b->copyFilters(uuid); });
		QAction *pf = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.PasteFilters")));
		pf->setEnabled(b->hasCopiedFilters());
		connect(pf, &QAction::triggered, this, [b, uuid] { b->pasteFilters(uuid); });
		QAction *flt = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Filters")));
		connect(flt, &QAction::triggered, this, [uuid] {
			obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (s) {
				obs_frontend_open_source_filters(s);
				obs_source_release(s);
			}
		});
		QAction *shot = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Screenshot")));
		connect(shot, &QAction::triggered, this, [uuid] {
			obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (s) {
				obs_frontend_take_source_screenshot(s);
				obs_source_release(s);
			}
		});
		menu.addSeparator();

		// 投影：按名字开，不区分主/副画布（obs_frontend_open_projector 只按名字找，无画布语义）
		obs_source_t *src = obs_get_source_by_uuid(uuid.toUtf8().constData());
		const QString srcName = src ? QString::fromUtf8(obs_source_get_name(src)) : QString();
		if (src)
			obs_source_release(src);
		QAction *winProj =
			menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.WindowProjector")));
		connect(winProj, &QAction::triggered, this,
			[srcName] { obs_frontend_open_projector("Scene", -1, nullptr, srcName.toUtf8().constData()); });
		QMenu *fsProj =
			menu.addMenu(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.FullscreenProjector")));
		const auto screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); ++i) {
			QScreen *sc = screens[i];
			QAction *a = fsProj->addAction(QStringLiteral("%1: %2 %3x%4")
							       .arg(i + 1)
							       .arg(sc->name())
							       .arg(sc->geometry().width())
							       .arg(sc->geometry().height()));
			connect(a, &QAction::triggered, this, [srcName, i] {
				obs_frontend_open_projector("Scene", i, nullptr, srcName.toUtf8().constData());
			});
		}
		menu.addSeparator();

		{
			// 转场覆盖（灰区：private settings 前端约定 key，README 已声明）
			QMenu *tm =
				menu.addMenu(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.TransitionOverride")));
			obs_source_t *s2 = obs_get_source_by_uuid(uuid.toUtf8().constData());
			obs_data_t *priv = obs_source_get_private_settings(s2);
			obs_data_set_default_int(priv, "transition_duration", 300);
			const QString cur = QString::fromUtf8(obs_data_get_string(priv, "transition"));
			auto addTr = [tm, uuid, cur, this](const QString &label, const QString &value) {
				QAction *a = tm->addAction(label);
				a->setCheckable(true);
				a->setChecked(value == cur);
				connect(a, &QAction::triggered, this, [uuid, value] {
					obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
					if (!s)
						return;
					obs_data_t *p = obs_source_get_private_settings(s);
					obs_data_set_string(p, "transition", value.toUtf8().constData());
					obs_data_release(p);
					obs_source_release(s);
				});
			};
			addTr(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.NoOverride")), QString());
			struct obs_frontend_source_list tl = {};
			obs_frontend_get_transitions(&tl);
			for (size_t i = 0; i < tl.sources.num; i++) {
				const QString n = QString::fromUtf8(obs_source_get_name(tl.sources.array[i]));
				addTr(n, n);
			}
			obs_frontend_source_list_free(&tl);
			auto *spin = new QSpinBox(tm);
			spin->setRange(50, 20000);
			spin->setSingleStep(50);
			spin->setSuffix(QStringLiteral(" ms"));
			spin->setValue((int)obs_data_get_int(priv, "transition_duration"));
			connect(spin, &QSpinBox::valueChanged, this, [uuid](int v) {
				obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
				if (!s)
					return;
				obs_data_t *p = obs_source_get_private_settings(s);
				obs_data_set_int(p, "transition_duration", v);
				obs_data_release(p);
				obs_source_release(s);
			});
			auto *wa = new QWidgetAction(tm);
			wa->setDefaultWidget(spin);
			tm->addSeparator();
			tm->addAction(wa);
			obs_data_release(priv);
			obs_source_release(s2);

			// 多画面显隐（README 声明：多画面开着时延迟生效，无公开刷新入口）
			QAction *mv =
				menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.ShowInMultiview")));
			mv->setCheckable(true);
			obs_source_t *s3 = obs_get_source_by_uuid(uuid.toUtf8().constData());
			obs_data_t *priv3 = obs_source_get_private_settings(s3);
			obs_data_set_default_bool(priv3, "show_in_multiview", true);
			mv->setChecked(obs_data_get_bool(priv3, "show_in_multiview"));
			obs_data_release(priv3);
			obs_source_release(s3);
			connect(mv, &QAction::triggered, this, [uuid](bool on) {
				obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
				if (!s)
					return;
				obs_data_t *p = obs_source_get_private_settings(s);
				obs_data_set_bool(p, "show_in_multiview", on);
				obs_data_release(p);
				obs_source_release(s);
			});
		}
		menu.addSeparator();
		addColorMenu(it);
	} else if (kind == RowPlan::Folder) {
		const QString cv = it->data(RoleCanvas).toString();
		const NodePath p = pathOfItem(it);
		QAction *addSub = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.AddSubfolder")));
		connect(addSub, &QAction::triggered, this, [b, cv, p] {
			b->applyTreeOp(obs_module_text("SceneAnchor.Undo.AddFolder"), [&] {
				return b->store.insertFolder(
					cv, p, INT_MAX, QString::fromUtf8(obs_module_text("SceneAnchor.NewFolder")));
			});
		});
		// fix round 1 Important：同场景分支，不捕获 pi，按 (cv, p) 在点击时重新定位。
		QAction *ren = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Rename")));
		connect(ren, &QAction::triggered, this, [this, cv, p] {
			const QModelIndex idx = findFolderIndex(cv, p);
			if (idx.isValid())
				view_->edit(idx);
		});
		// 「解散」与「删除」在最常见的情形（顶层文件夹只装场景）下结果看着一样，
		// 差别要到有嵌套子文件夹时才显现。菜单项本身说不清，挂 tooltip 说明后果。
		menu.setToolTipsVisible(true);
		QAction *diss = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Dissolve")));
		diss->setToolTip(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.DissolveTip")));
		connect(diss, &QAction::triggered, this, [b, cv, p] {
			b->applyTreeOp(obs_module_text("SceneAnchor.Undo.Dissolve"),
				       [&] { return b->store.dissolveFolder(cv, p); });
		});
		QAction *rm = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.RemoveFolder")));
		rm->setToolTip(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.RemoveFolderTip")));
		connect(rm, &QAction::triggered, this, [b, cv, p] {
			b->applyTreeOp(obs_module_text("SceneAnchor.Undo.RemoveFolder"),
				       [&] { return b->store.removeNode(cv, p); });
		});
		menu.addSeparator();
		addColorMenu(it);
	} else {
		// 空白区
		QAction *addF = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.AddFolder")));
		connect(addF, &QAction::triggered, btnAddFolder_, &QToolButton::click);
		QAction *addS = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.AddScene")));
		connect(addS, &QAction::triggered, btnAddScene_, &QToolButton::click);
		menu.addSeparator();
		QMenu *dc = menu.addMenu(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.DoubleClick")));
		const QString cur = b->doubleClickMode();
		for (const auto &[key, label] :
		     {std::pair<QString, QString>{QStringLiteral("transition"),
						  QString::fromUtf8(obs_module_text("SceneAnchor.DC.Transition"))},
		      {QStringLiteral("rename"), QString::fromUtf8(obs_module_text("SceneAnchor.DC.Rename"))},
		      {QStringLiteral("none"), QString::fromUtf8(obs_module_text("SceneAnchor.DC.None"))}}) {
			QAction *a = dc->addAction(label);
			a->setCheckable(true);
			a->setChecked(key == cur);
			const QString k = key;
			connect(a, &QAction::triggered, this, [b, k] { b->setDoubleClickMode(k); });
		}

		// 「选中即切换」与双击动作并列——两者都是"这个手势做什么"，属同一类。
		QAction *sel = menu.addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Opt.SelectSwitches")));
		sel->setCheckable(true);
		sel->setChecked(b->option(kOptSelectSwitches.key, kOptSelectSwitches.def));
		connect(sel, &QAction::triggered, this, [b](bool on) { b->setOption(kOptSelectSwitches.key, on); });

		// 显示类选项单开一组：改的是画什么，不是手势做什么。两项都要 rebuild/refresh 才可见。
		QMenu *disp = menu.addMenu(QString::fromUtf8(obs_module_text("SceneAnchor.Menu.Display")));
		QAction *mru = disp->addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Opt.ShowMru")));
		mru->setCheckable(true);
		mru->setChecked(b->option(kOptShowMru.key, kOptShowMru.def));
		connect(mru, &QAction::triggered, this, [this, b](bool on) {
			b->setOption(kOptShowMru.key, on);
			refreshMru();
		});
		QAction *ico = disp->addAction(QString::fromUtf8(obs_module_text("SceneAnchor.Opt.SceneIcons")));
		ico->setCheckable(true);
		ico->setChecked(b->option(kOptIcons.key, kOptIcons.def));
		connect(ico, &QAction::triggered, this, [this, b](bool on) {
			b->setOption(kOptIcons.key, on);
			rebuild();
		});
	}
	menu.exec(view_->viewport()->mapToGlobal(pos));
}
