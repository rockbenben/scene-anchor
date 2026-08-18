// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "tree_store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <set>

static QJsonObject nodeToJson(const TreeNode &n)
{
	QJsonObject o;
	if (n.type == TreeNode::Folder) {
		o["t"] = "folder";
		o["name"] = n.name;
		o["expanded"] = n.expanded;
		QJsonArray kids;
		for (const auto &c : n.children)
			kids.append(nodeToJson(*c));
		o["children"] = kids;
	} else {
		o["t"] = "scene";
		o["uuid"] = n.uuid;
		if (!n.name.isEmpty())
			o["name"] = n.name;
	}
	if (!n.color.isEmpty())
		o["color"] = n.color;
	return o;
}

static std::unique_ptr<TreeNode> nodeFromJson(const QJsonObject &o)
{
	auto n = std::make_unique<TreeNode>();
	const QString t = o["t"].toString();
	n->color = o["color"].toString();
	if (t == QLatin1String("folder")) {
		n->type = TreeNode::Folder;
		n->name = o["name"].toString();
		n->expanded = o["expanded"].toBool(true);
		for (const auto v : o["children"].toArray())
			if (v.isObject())
				if (auto c = nodeFromJson(v.toObject()))
					n->children.push_back(std::move(c));
	} else if (t == QLatin1String("scene")) {
		n->type = TreeNode::Scene;
		n->uuid = o["uuid"].toString();
		if (n->uuid.isEmpty())
			return nullptr;
		n->name = o["name"].toString();
	} else {
		return nullptr;
	}
	return n;
}

TreeNode *TreeStore::mutableNodeAt(const QString &canvas, const NodePath &path)
{
	if (foreign_)
		return nullptr;
	auto it = roots_.find(canvas);
	if (it == roots_.end()) {
		// 不存在的 canvas 上，非空路径必然非法 —— 此时绝不能建条目。
		// 用 roots_[canvas] 会因 map::operator[] 自动建值，让一个「返回 false 的失败操作」
		// 也改变 toJson() 输出，违反头文件契约，并让 Task 6 的 applyTreeOp
		// （靠 toJson() 前后比对判断是否真的变了）为失败操作记下空的 undo 条目。
		if (!path.empty())
			return nullptr;
		it = roots_.emplace(canvas, TreeNode{}).first; // 仅根路径：调用方后续必定成功
	}
	TreeNode *n = &it->second;
	for (int idx : path) {
		if (n->type != TreeNode::Folder || idx < 0 || idx >= (int)n->children.size())
			return nullptr;
		n = n->children[idx].get();
	}
	return n;
}

const TreeNode *TreeStore::nodeAt(const QString &canvas, const NodePath &path) const
{
	auto it = roots_.find(canvas);
	if (it == roots_.end())
		return nullptr;
	const TreeNode *n = &it->second;
	for (int idx : path) {
		if (n->type != TreeNode::Folder || idx < 0 || idx >= (int)n->children.size())
			return nullptr;
		n = n->children[idx].get();
	}
	return n;
}

const std::vector<std::unique_ptr<TreeNode>> *TreeStore::canvasRoot(const QString &canvas) const
{
	auto it = roots_.find(canvas);
	return it == roots_.end() ? nullptr : &it->second.children;
}

bool TreeStore::insertFolder(const QString &canvas, const NodePath &parent, int index, const QString &name)
{
	TreeNode *p = mutableNodeAt(canvas, parent);
	if (!p || p->type != TreeNode::Folder)
		return false;
	auto n = std::make_unique<TreeNode>();
	n->type = TreeNode::Folder;
	n->name = name;
	const int i = std::clamp(index, 0, (int)p->children.size());
	p->children.insert(p->children.begin() + i, std::move(n));
	return true;
}

bool TreeStore::renameFolder(const QString &canvas, const NodePath &path, const QString &name)
{
	if (path.empty() || name.isEmpty())
		return false;
	TreeNode *n = mutableNodeAt(canvas, path);
	if (!n || n->type != TreeNode::Folder)
		return false;
	n->name = name;
	return true;
}

bool TreeStore::dissolveFolder(const QString &canvas, const NodePath &path)
{
	if (path.empty())
		return false;
	TreeNode *n = mutableNodeAt(canvas, path);
	if (!n || n->type != TreeNode::Folder)
		return false;
	NodePath parentPath(path.begin(), path.end() - 1);
	TreeNode *p = mutableNodeAt(canvas, parentPath);
	if (!p)
		return false; // 不变量保证不会发生，但它是隐式的
	const int idx = path.back();
	auto folder = std::move(p->children[idx]);
	p->children.erase(p->children.begin() + idx);
	for (size_t i = 0; i < folder->children.size(); ++i)
		p->children.insert(p->children.begin() + idx + i, std::move(folder->children[i]));
	return true;
}

bool TreeStore::removeNode(const QString &canvas, const NodePath &path)
{
	if (path.empty())
		return false;
	if (!mutableNodeAt(canvas, path))
		return false;
	NodePath parentPath(path.begin(), path.end() - 1);
	TreeNode *p = mutableNodeAt(canvas, parentPath);
	if (!p)
		return false; // 不变量保证不会发生，但它是隐式的
	p->children.erase(p->children.begin() + path.back());
	return true;
}

static bool isPrefixOf(const NodePath &a, const NodePath &b)
{ // a 是 b 的前缀（含相等）
	return a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool TreeStore::moveNodes(const QString &canvas, std::vector<NodePath> sources, const NodePath &destFolder,
			  int destIndex, int *insertedAt, int *movedCount)
{
	if (foreign_ || sources.empty())
		return false;
	// 用 nodeAt（const，不建条目）做存在性检查：mutableNodeAt 对空路径 + 不存在的 canvas
	// 会自动建根条目（见其注释），若这里直接用它，一次注定失败的 move（比如源节点根本
	// 不存在）也会先把目标 canvas 凭空建出来，让"失败必须不留痕"（D-5）破功。
	const TreeNode *destCheck = nodeAt(canvas, destFolder);
	if (!destCheck || destCheck->type != TreeNode::Folder)
		return false;
	std::sort(sources.begin(), sources.end()); // 文档序
	std::vector<NodePath> tops;                // 祖先吞并
	for (const auto &s : sources) {
		if (s.empty())
			return false; // 根不可移
		if (!tops.empty() && isPrefixOf(tops.back(), s))
			continue;
		tops.push_back(s);
	}
	for (const auto &s : tops) {
		if (isPrefixOf(s, destFolder))
			return false; // 不能拖进自身子树
		if (!nodeAt(canvas, s))
			return false; // 全部先验存在，避免半程失败
		NodePath par(s.begin(), s.end() - 1);
		if (!nodeAt(canvas, par))
			return false; // D-3 纵深防御：上面的 nodeAt(canvas, s) 已证明
				      // s 本身可解析，其前缀 par 必然也可解析，这里目前不可达
	}
	TreeNode *dest = mutableNodeAt(canvas, destFolder); // 先验已过，canvas 确认存在，不会触发自动建条目
	if (!dest)
		return false;
	int adjusted = std::clamp(destIndex, 0, (int)dest->children.size());
	// 阈值必须在循环前冻结：拿源索引跟正在缩小的 adjusted 比会漏掉递减。
	// 反例 root=[w,x,y]，moveNodes({{0},{1}}, {}, 2)（应为空操作）：
	// {0} 让 adjusted 2→1，随后 {1} 因 1<1 不成立而被漏掉，结果错成 [y,w,x]。
	const int threshold = adjusted;
	for (const auto &s : tops) { // 同父且在插入点之前的被移走 → 索引前移
		NodePath par(s.begin(), s.end() - 1);
		if (par == destFolder && s.back() < threshold)
			--adjusted;
	}
	std::vector<std::unique_ptr<TreeNode>> grabbed(tops.size());
	for (int i = (int)tops.size() - 1; i >= 0; --i) { // 逆文档序摘除，前面路径索引不受影响
		const NodePath &s = tops[i];
		NodePath par(s.begin(), s.end() - 1);
		TreeNode *p = mutableNodeAt(canvas, par);
		grabbed[i] = std::move(p->children[s.back()]);
		p->children.erase(p->children.begin() + s.back());
	}
	for (size_t i = 0; i < grabbed.size(); ++i) // dest 是堆上节点，指针稳定
		dest->children.insert(dest->children.begin() + adjusted + i, std::move(grabbed[i]));
	if (insertedAt)
		*insertedAt = adjusted; // 真实插入点，非调用方传入的 destIndex
	if (movedCount)
		*movedCount = (int)grabbed.size(); // 祖先吞并后的真实数量
	return true;
}

static bool dfsFind(const TreeNode &folder, const QString &uuid, NodePath &path)
{
	for (int i = 0; i < (int)folder.children.size(); ++i) {
		const TreeNode &n = *folder.children[i];
		path.push_back(i);
		if (n.type == TreeNode::Scene && n.uuid == uuid)
			return true;
		if (n.type == TreeNode::Folder && dfsFind(n, uuid, path))
			return true;
		path.pop_back();
	}
	return false;
}

std::optional<NodePath> TreeStore::findScene(const QString &canvas, const QString &sceneUuid) const
{
	auto it = roots_.find(canvas);
	if (it == roots_.end())
		return std::nullopt;
	NodePath p;
	if (dfsFind(it->second, sceneUuid, p))
		return p;
	return std::nullopt;
}

bool TreeStore::placeScene(const QString &canvas, const QString &sceneUuid, const NodePath &destFolder, int destIndex)
{
	if (foreign_ || sceneUuid.isEmpty())
		return false;
	if (auto existing = findScene(canvas, sceneUuid))
		return moveNodes(canvas, {*existing}, destFolder, destIndex);
	TreeNode *dest = mutableNodeAt(canvas, destFolder);
	if (!dest || dest->type != TreeNode::Folder)
		return false;
	auto n = std::make_unique<TreeNode>();
	n->type = TreeNode::Scene;
	n->uuid = sceneUuid;
	const int i = std::clamp(destIndex, 0, (int)dest->children.size());
	dest->children.insert(dest->children.begin() + i, std::move(n));
	return true;
}

bool TreeStore::setColor(const QString &canvas, const NodePath &path, const QString &color)
{
	if (path.empty())
		return false;
	TreeNode *n = mutableNodeAt(canvas, path);
	if (!n)
		return false;
	n->color = color;
	return true;
}

bool TreeStore::setExpanded(const QString &canvas, const NodePath &path, bool expanded)
{
	if (path.empty())
		return false;
	TreeNode *n = mutableNodeAt(canvas, path);
	if (!n || n->type != TreeNode::Folder)
		return false;
	n->expanded = expanded;
	return true;
}

void TreeStore::touchMru(const QString &sceneUuid, int cap)
{
	if (foreign_ || sceneUuid.isEmpty())
		return;
	mru_.removeAll(sceneUuid);
	mru_.prepend(sceneUuid);
	while (mru_.size() > cap)
		mru_.removeLast();
}

static void pruneScenesRec(TreeNode &folder, const QSet<QString> &live)
{
	auto &v = folder.children;
	v.erase(std::remove_if(v.begin(), v.end(),
			       [&](const std::unique_ptr<TreeNode> &c) {
				       return c->type == TreeNode::Scene && !live.contains(c->uuid);
			       }),
		v.end());
	for (auto &c : v)
		if (c->type == TreeNode::Folder)
			pruneScenesRec(*c, live);
}

static void stampRec(TreeNode &folder, const std::map<QString, QString> &uuidToName)
{
	for (auto &c : folder.children) {
		if (c->type == TreeNode::Scene) {
			if (auto it = uuidToName.find(c->uuid); it != uuidToName.end())
				c->name = it->second;
		} else {
			stampRec(*c, uuidToName);
		}
	}
}

void TreeStore::stampSceneNames(const std::map<QString, QString> &uuidToName)
{
	if (foreign_)
		return;
	for (auto &[uuid, root] : roots_)
		stampRec(root, uuidToName);
}

// 解析必须两趟。单趟会让靠前的「uuid 已死、名字命中」节点抢走靠后节点用 uuid 正当持有的场景：
// 两个分支的检查不对称 —— 名字回退查 claimed，uuid 主键分支却无条件 claim。
// 结果两个节点指向同一 uuid，而 pruneScenesRec 认为两者都活、一个都不清，
// store 里就永久留下重复 uuid 的幽灵。触发路径极普通：删掉场景 X、再建一个同名的新场景。
static void claimLiveRec(const TreeNode &folder, const QSet<QString> &liveScenes, QSet<QString> &claimed)
{
	for (const auto &c : folder.children) {
		if (c->type == TreeNode::Folder)
			claimLiveRec(*c, liveScenes, claimed);
		else if (liveScenes.contains(c->uuid))
			claimed.insert(c->uuid);
	}
}

static void resolveRec(TreeNode &folder, const QSet<QString> &liveScenes, const std::map<QString, QString> &liveNames,
		       QSet<QString> &claimed)
{
	for (auto &c : folder.children) {
		if (c->type == TreeNode::Folder) {
			resolveRec(*c, liveScenes, liveNames, claimed);
			continue;
		}
		if (liveScenes.contains(c->uuid))
			continue; // 第一趟已 claim
		if (c->name.isEmpty())
			continue; // 旧数据无名字，不参与回退
		if (auto it = liveNames.find(c->name); it != liveNames.end() && !claimed.contains(it->second)) {
			c->uuid = it->second; // 自愈：复制集合后 uuid 变了，按名字认回来
			claimed.insert(c->uuid);
		}
	}
}

void TreeStore::resolveAndPrune(const std::vector<LiveCanvas> &live)
{
	if (foreign_)
		return;
	// 保命闸：OBS 永远至少有一个主画布，空活跃集绝不可能是合法状态 —— 只可能是调用方出错。
	// 此时若照常执行，会把每个画布的根都当作「画布已不存在」抹除，用户文件夹全毁。
	// 宁可什么都不做：下一次正常的加载事件会重新解析。
	if (live.empty())
		return;
	// 按 canvas 分区：每个画布只认自己的活跃场景，杜绝跨画布幽灵与名字盗用
	std::map<QString, QSet<QString>> scenesByCanvas;
	std::map<QString, std::map<QString, QString>> namesByCanvas;
	QSet<QString> allLive;
	for (const auto &cv : live) {
		auto &ss = scenesByCanvas[cv.uuid];
		auto &nn = namesByCanvas[cv.uuid];
		for (const auto &sc : cv.scenes) {
			ss.insert(sc.uuid);
			allLive.insert(sc.uuid);
			if (!sc.name.isEmpty())
				nn[sc.name] = sc.uuid;
		}
	}
	for (auto it = roots_.begin(); it != roots_.end();) {
		auto sIt = scenesByCanvas.find(it->first);
		if (sIt == scenesByCanvas.end()) {
			it = roots_.erase(it);
			continue;
		} // 画布已不存在
		const QSet<QString> &liveScenes = sIt->second;
		QSet<QString> claimed;
		claimLiveRec(it->second, liveScenes, claimed);                         // 第一趟：uuid 正当持有者先占位
		resolveRec(it->second, liveScenes, namesByCanvas[it->first], claimed); // 第二趟：名字回退
		pruneScenesRec(it->second, liveScenes);                                // 清除仍未解析的
		++it;
	}
	QStringList kept;
	for (const auto &u : mru_)
		if (allLive.contains(u))
			kept << u; // MRU 是全局的，用合集
	mru_ = kept;
}

void TreeStore::clear()
{
	roots_.clear();
	mru_.clear();
	foreign_ = false;
	rawForeign_.clear();
}

QString TreeStore::toJson() const
{
	if (foreign_)
		return rawForeign_;
	QJsonObject o;
	o["version"] = kTreeStoreVersion;
	QJsonObject cs;
	// 结构按画布 uuid 分区保留：树本来就是这么组织的，且不绑死"只有一个画布"这个当前事实。
	for (const auto &[uuid, root] : roots_) {
		QJsonArray tree;
		for (const auto &c : root.children)
			tree.append(nodeToJson(*c));
		QJsonObject co;
		co["tree"] = tree;
		cs[uuid] = co;
	}
	o["canvases"] = cs;
	QJsonArray mru;
	for (const auto &u : mru_)
		mru.append(u);
	o["mru"] = mru;
	return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

bool TreeStore::fromJson(const QString &json)
{
	clear();
	QJsonParseError err{};
	const auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return false;
	const QJsonObject o = doc.object();
	if (o["version"].toInt(1) > kTreeStoreVersion) {
		foreign_ = true;
		rawForeign_ = json;
		return true;
	}
	const QJsonObject cs = o["canvases"].toObject();
	for (auto it = cs.begin(); it != cs.end(); ++it) {
		TreeNode root;
		for (const auto nv : it.value().toObject()["tree"].toArray())
			if (nv.isObject())
				if (auto n = nodeFromJson(nv.toObject()))
					root.children.push_back(std::move(n));
		// 旧版本写过 co["expanded"]（画布标题行的折叠态）。标题行已不存在，读到就忽略。
		roots_.emplace(it.key(), std::move(root));
	}
	for (const auto v : o["mru"].toArray()) {
		const QString s = v.toString();
		if (!s.isEmpty())
			mru_ << s;
	}
	return true;
}
