// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/tree_store.h"
#include "../src/projection.h"
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)

// 空安全解引用：查找失败时返回一个空节点，让断言输出 FAIL 而不是让整个测试进程崩溃。
// 没有它的话，实现一旦回归，nodeAt 返回 nullptr，测试是段错误退出，看不到是哪条断言挂的。
// 所有对 nodeAt()/child 指针的解引用都必须经过 N()。
static const TreeNode &N(const TreeNode *p)
{
	static const TreeNode kEmpty{};
	return p ? *p : kEmpty;
}

// 同类越界安全取值器：plan.size() 断言失败后，后续 plan[i] 会越界崩溃，
// 丢掉全部诊断输出。所有对 plan 的索引访问都必须经过 R()。
static const RowPlan &R(const std::vector<RowPlan> &v, size_t i)
{
	static const RowPlan kEmpty{};
	return i < v.size() ? v[i] : kEmpty;
}

// 空安全：canvas 不存在时返回空向量，避免断言失败演变成崩溃。
static const std::vector<std::unique_ptr<TreeNode>> &C(const std::vector<std::unique_ptr<TreeNode>> *p)
{
	static const std::vector<std::unique_ptr<TreeNode>> kEmpty{};
	return p ? *p : kEmpty;
}

static void test_roundtrip()
{
	TreeStore s;
	CHECK(s.insertFolder("cv1", {}, 0, "开场"));
	CHECK(s.placeScene("cv1", "uuid-a", {0}, 0));
	CHECK(s.placeScene("cv1", "uuid-b", {}, 1));
	CHECK(s.setColor("cv1", {0}, "#d13438"));
	CHECK(s.setExpanded("cv1", {0}, false));
	s.touchMru("uuid-a", 5);

	const QString json = s.toJson();
	TreeStore t;
	CHECK(t.fromJson(json));
	CHECK(t.toJson() == json); // 往返稳定
	const TreeNode *f = t.nodeAt("cv1", {0});
	CHECK(f && f->type == TreeNode::Folder && f->name == QStringLiteral("开场"));
	CHECK(N(f).color == QStringLiteral("#d13438") && N(f).expanded == false);
	const TreeNode *sc = t.nodeAt("cv1", {0, 0});
	CHECK(N(sc).type == TreeNode::Scene && N(sc).uuid == QStringLiteral("uuid-a"));
	CHECK(t.mru() == QStringList{QStringLiteral("uuid-a")});
}

static void test_bad_json()
{
	TreeStore s;
	CHECK(!s.fromJson("not json"));
	CHECK(s.canvasRoot("cv1") == nullptr); // 已清空
	CHECK(!s.fromJson("[1,2,3]"));         // 非 object
	CHECK(s.fromJson("{}"));               // 空 object = 合法空树
}

static void test_foreign_version()
{
	const QString v9 = QStringLiteral("{\"version\":9,\"future\":true}");
	TreeStore s;
	CHECK(s.fromJson(v9));
	CHECK(s.isForeign());
	CHECK(s.toJson() == v9);                   // 原样写回，一字不动
	CHECK(!s.insertFolder("cv1", {}, 0, "x")); // foreign 态所有变更器拒绝
	CHECK(!s.placeScene("cv1", "u", {}, 0));
	s.resolveAndPrune({}); // no-op 不崩
	CHECK(s.toJson() == v9);
}

// 失败的操作绝不能改变 store。Task 6 的 applyTreeOp 靠 toJson() 前后比对
// 判断"是否真的变了"，一旦失败操作也留下痕迹，被拒绝的拖拽会记下空的 undo 条目。
static void test_failed_op_leaves_store_untouched()
{
	TreeStore s;
	const QString before = s.toJson();
	CHECK(!s.insertFolder("cv1", NodePath{99}, 0, "x")); // canvas 不存在 + 坏路径
	CHECK(!s.setColor("cv2", NodePath{5}, "#fff"));      // 同上
	CHECK(!s.setExpanded("cv3", NodePath{0}, true));
	CHECK(!s.placeScene("cv4", "u", NodePath{7}, 0));
	CHECK(s.canvasRoot("cv1") == nullptr);
	CHECK(s.canvasRoot("cv2") == nullptr);
	CHECK(s.canvasRoot("cv3") == nullptr);
	CHECK(s.canvasRoot("cv4") == nullptr);
	CHECK(s.toJson() == before); // 关键：输出一字未变

	// 反面：根路径上的合法操作确实会建 canvas 并改变输出
	CHECK(s.insertFolder("cv1", {}, 0, "A"));
	CHECK(s.canvasRoot("cv1") != nullptr);
	CHECK(s.toJson() != before);
}

static TreeStore makeFixture()
{
	// cv1: [F"A"[S(a), F"B"[S(b)]], S(c)]
	TreeStore s;
	s.insertFolder("cv1", {}, 0, "A");
	s.placeScene("cv1", "a", {0}, 0);
	s.insertFolder("cv1", {0}, 1, "B");
	s.placeScene("cv1", "b", {0, 1}, 0);
	s.placeScene("cv1", "c", {}, 1);
	return s;
}

static void test_mutators()
{
	{
		TreeStore s = makeFixture();
		CHECK(s.renameFolder("cv1", {0}, "A2"));
		CHECK(N(s.nodeAt("cv1", {0})).name == QStringLiteral("A2"));
		CHECK(!s.renameFolder("cv1", {0, 0}, "x")); // scene 不能 renameFolder
		CHECK(!s.renameFolder("cv1", {9}, "x"));    // 坏路径
	}
	{
		TreeStore s = makeFixture();
		CHECK(s.dissolveFolder("cv1", {0, 1})); // B 解散：b 上移到 A 内 B 的位置
		CHECK(N(s.nodeAt("cv1", {0, 1})).uuid == QStringLiteral("b"));
		CHECK(s.dissolveFolder("cv1", {0})); // A 解散：a、b 到根部原位
		CHECK(N(s.nodeAt("cv1", {0})).uuid == QStringLiteral("a"));
		CHECK(N(s.nodeAt("cv1", {1})).uuid == QStringLiteral("b"));
		CHECK(N(s.nodeAt("cv1", {2})).uuid == QStringLiteral("c"));
	}
	{
		TreeStore s = makeFixture();
		CHECK(s.removeNode("cv1", {0})); // 整个 A 子树移出 store
		CHECK(!s.findScene("cv1", "a"));
		CHECK(!s.findScene("cv1", "b"));
		CHECK(s.findScene("cv1", "c").value() == NodePath{0});
		CHECK(!s.removeNode("cv1", {})); // 根不可删
	}
}

static void test_move()
{
	{ // 多选保持文档序、同父前置索引修正：把 a(在A内) 和 c(根1) 移到根 0
		TreeStore s = makeFixture();
		CHECK(s.moveNodes("cv1", {{0, 0}, {1}}, {}, 0));
		CHECK(N(s.nodeAt("cv1", {0})).uuid == QStringLiteral("a"));
		CHECK(N(s.nodeAt("cv1", {1})).uuid == QStringLiteral("c"));
		CHECK(N(s.nodeAt("cv1", {2})).name == QStringLiteral("A"));
	}
	{ // 选中文件夹+其子项：子项被祖先吞并，整树只动一次
		TreeStore s = makeFixture();
		CHECK(s.moveNodes("cv1", {{0}, {0, 1}}, {}, 2));
		CHECK(N(s.nodeAt("cv1", {0})).uuid == QStringLiteral("c"));
		CHECK(N(s.nodeAt("cv1", {1})).name == QStringLiteral("A"));
		CHECK(N(s.nodeAt("cv1", {1, 1})).name == QStringLiteral("B")); // B 仍在 A 内
	}
	{ // 拖进自己的子树 → 拒绝
		TreeStore s = makeFixture();
		CHECK(!s.moveNodes("cv1", {{0}}, {0, 1}, 0));
	}
	{ // 同父多源且都在插入点之前 —— 阈值若不冻结会漏递减（Task 4 审查发现的 Critical）
		TreeStore s;
		CHECK(s.placeScene("cv1", "w", {}, 0));
		CHECK(s.placeScene("cv1", "x", {}, 1));
		CHECK(s.placeScene("cv1", "y", {}, 2));
		CHECK(s.moveNodes("cv1", {{0}, {1}}, {}, 2)); // w、x 拖到 y 之前 = 空操作
		CHECK(N(s.nodeAt("cv1", {0})).uuid == QStringLiteral("w"));
		CHECK(N(s.nodeAt("cv1", {1})).uuid == QStringLiteral("x"));
		CHECK(N(s.nodeAt("cv1", {2})).uuid == QStringLiteral("y"));
	}
	{ // 三源跨越一个非源节点，全部在插入点之前
		TreeStore s;
		CHECK(s.placeScene("cv1", "w", {}, 0));
		CHECK(s.placeScene("cv1", "x", {}, 1));
		CHECK(s.insertFolder("cv1", {}, 2, "D"));
		CHECK(s.placeScene("cv1", "y", {}, 3));
		CHECK(s.placeScene("cv1", "z", {}, 4));
		CHECK(s.moveNodes("cv1", {{0}, {1}, {3}}, {}, 4)); // w、x、y 移到 z 之前
		CHECK(N(s.nodeAt("cv1", {0})).name == QStringLiteral("D"));
		CHECK(N(s.nodeAt("cv1", {1})).uuid == QStringLiteral("w"));
		CHECK(N(s.nodeAt("cv1", {2})).uuid == QStringLiteral("x"));
		CHECK(N(s.nodeAt("cv1", {3})).uuid == QStringLiteral("y"));
		CHECK(N(s.nodeAt("cv1", {4})).uuid == QStringLiteral("z"));
	}
	{ // 移入目标是自己前面的兄弟被移走：dest 指针稳定性
		TreeStore s = makeFixture();
		CHECK(s.moveNodes("cv1", {{1}}, {0, 1}, 0)); // c 移入 B
		CHECK(N(s.nodeAt("cv1", {0, 1, 0})).uuid == QStringLiteral("c"));
	}
	{ // placeScene 对已存在场景 = 移动（保颜色）
		TreeStore s = makeFixture();
		s.setColor("cv1", {0, 0}, "#107c10");
		CHECK(s.placeScene("cv1", "a", {}, 2));
		CHECK(N(s.nodeAt("cv1", {2})).uuid == QStringLiteral("a"));
		CHECK(N(s.nodeAt("cv1", {2})).color == QStringLiteral("#107c10"));
		CHECK(s.findScene("cv1", "a") == std::optional<NodePath>{NodePath{2}}); // 移动而非复制：只在新位置
		CHECK(N(s.nodeAt("cv1", {0})).children.size() == 1);                    // A 里只剩 B
	}
	{ // moveNodes 必须报告真实插入点与真实移动数 —— 调用方据此串接后续插入。
		// 反例见 R-19：拿原始 destIndex + sources.size() 推算会让后续项落错位置。
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "F"));
		CHECK(s.placeScene("cv1", "a", {0}, 0));
		CHECK(s.placeScene("cv1", "b", {0}, 1));
		int at = -1, n = -1;
		CHECK(s.moveNodes("cv1", {{0, 0}}, {0}, 1, &at, &n)); // 把 a 移到 b 之前（同父、在目标之前）
		CHECK(at == 0);                                       // 插入点被下调，不是传入的 1
		CHECK(n == 1);
	}
	{ // 祖先吞并后 movedCount 应小于传入的 sources 数
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "outer"));
		CHECK(s.insertFolder("cv1", {0}, 0, "inner"));
		CHECK(s.placeScene("cv1", "x", {0, 0}, 0));
		CHECK(s.insertFolder("cv1", {}, 1, "dest"));
		int at = -1, n = -1;
		CHECK(s.moveNodes("cv1", {{0}, {0, 0}}, {1}, 0, &at, &n)); // 选中 outer 与其子 inner
		CHECK(n == 1);                                             // inner 被祖先吞并
	}
}

static void test_prune()
{
	TreeStore s = makeFixture();
	s.touchMru("a", 5);
	s.touchMru("dead", 5);
	s.insertFolder("cv-gone", {}, 0, "X");
	s.resolveAndPrune({{QStringLiteral("cv1"),
			    QStringLiteral("主"),
			    {{QStringLiteral("a"), QString()}, {QStringLiteral("c"), QString()}}}});
	CHECK(!s.findScene("cv1", "b")); // 死场景清掉
	CHECK(s.findScene("cv1", "a"));
	CHECK(N(s.nodeAt("cv1", {0, 1})).name == QStringLiteral("B")); // 空文件夹保留
	CHECK(s.canvasRoot("cv-gone") == nullptr);                     // 死 canvas 整棵清掉
	CHECK(s.mru() == QStringList{QStringLiteral("a")});
}

static void test_edge_cases()
{
	{ // 根拒绝：Task 3 只断言了 removeNode，补另外两个
		TreeStore s = makeFixture();
		CHECK(!s.renameFolder("cv1", {}, "x"));
		CHECK(!s.dissolveFolder("cv1", {}));
	}
	{ // 空文件夹解散：循环退化为 0 次插入，不得崩溃或留下残骸
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "empty"));
		const QString before = s.toJson();
		CHECK(s.dissolveFolder("cv1", {0}));
		CHECK(C(s.canvasRoot("cv1")).empty());
		CHECK(s.toJson() != before);
	}
	{ // 两侧都有兄弟的中间元素 —— 最易 off-by-one 的形状，此前从未测过
		// cv1: [S(x), F"M"[S(p), S(q), S(r)], S(y)]
		TreeStore s;
		CHECK(s.placeScene("cv1", "x", {}, 0));
		CHECK(s.insertFolder("cv1", {}, 1, "M"));
		CHECK(s.placeScene("cv1", "p", {1}, 0));
		CHECK(s.placeScene("cv1", "q", {1}, 1));
		CHECK(s.placeScene("cv1", "r", {1}, 2));
		CHECK(s.placeScene("cv1", "y", {}, 2));
		CHECK(s.dissolveFolder("cv1", {1})); // 中间的 M 解散
		CHECK(N(s.nodeAt("cv1", {0})).uuid == QStringLiteral("x"));
		CHECK(N(s.nodeAt("cv1", {1})).uuid == QStringLiteral("p"));
		CHECK(N(s.nodeAt("cv1", {2})).uuid == QStringLiteral("q"));
		CHECK(N(s.nodeAt("cv1", {3})).uuid == QStringLiteral("r"));
		CHECK(N(s.nodeAt("cv1", {4})).uuid == QStringLiteral("y")); // 尾部兄弟正确后移
	}
	{ // removeNode 在非零索引 + 非根父路径（此前零覆盖）
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "F"));
		CHECK(s.placeScene("cv1", "a", {0}, 0));
		CHECK(s.placeScene("cv1", "b", {0}, 1));
		CHECK(s.placeScene("cv1", "c", {0}, 2));
		CHECK(s.removeNode("cv1", {0, 1})); // 摘掉中间的 b
		CHECK(N(s.nodeAt("cv1", {0, 0})).uuid == QStringLiteral("a"));
		CHECK(N(s.nodeAt("cv1", {0, 1})).uuid == QStringLiteral("c"));
		CHECK(N(s.nodeAt("cv1", {0})).children.size() == 2);
	}
	{ // 操作后祖先文件夹变空但仍存在（无级联删除是设计）
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "F"));
		CHECK(s.placeScene("cv1", "only", {0}, 0));
		CHECK(s.removeNode("cv1", {0, 0}));
		CHECK(N(s.nodeAt("cv1", {0})).type == TreeNode::Folder);
		CHECK(N(s.nodeAt("cv1", {0})).children.empty()); // 空但仍在
	}
	{ // 不存在的 canvas：三个变更器都不得建条目
		TreeStore s;
		const QString before = s.toJson();
		CHECK(!s.renameFolder("nope", {0}, "x"));
		CHECK(!s.dissolveFolder("nope", {0}));
		CHECK(!s.removeNode("nope", {0}));
		CHECK(!s.moveNodes("nope", {{0}}, {}, 0));
		CHECK(s.canvasRoot("nope") == nullptr);
		CHECK(s.toJson() == before);
	}
	{                                    // 边界索引 vs 远越界：同一路径，但都要拒绝且不留痕
		TreeStore s = makeFixture(); // 根有 2 个子节点
		const QString before = s.toJson();
		CHECK(!s.removeNode("cv1", {2}));  // idx == size()
		CHECK(!s.removeNode("cv1", {99})); // 远越界
		CHECK(s.toJson() == before);
	}
}

static void test_projection()
{
	{                                              // 单 canvas：无 header；僵尸跳过；未归类追加；名字取实时值
		TreeStore s = makeFixture();           // [A[a, B[b]], c] 且我们再造一个僵尸
		s.placeScene("cv1", "zombie", {0}, 0); // store 有、live 无
		std::vector<LiveCanvas> live{{QStringLiteral("cv1"),
					      QStringLiteral("主画布"),
					      {{QStringLiteral("a"), QStringLiteral("场景A")},
					       {QStringLiteral("b"), QStringLiteral("场景B")},
					       {QStringLiteral("c"), QStringLiteral("场景C")},
					       {QStringLiteral("free"), QStringLiteral("自由场景")}}}};
		auto plan = planProjection(s, live);
		// 期望顺序: F"A"(d0), a(d1), F"B"(d1), b(d2), c(d0), free(d0,unplaced)
		CHECK(plan.size() == 6);
		CHECK(R(plan, 0).kind == RowPlan::Folder && R(plan, 0).depth == 0 &&
		      R(plan, 0).name == QStringLiteral("A"));
		CHECK(R(plan, 1).kind == RowPlan::Scene && R(plan, 1).depth == 1 &&
		      R(plan, 1).name == QStringLiteral("场景A") && R(plan, 1).placed);
		CHECK(R(plan, 2).kind == RowPlan::Folder && R(plan, 2).name == QStringLiteral("B"));
		CHECK(R(plan, 3).uuid == QStringLiteral("b") && R(plan, 3).depth == 2);
		CHECK(R(plan, 4).uuid == QStringLiteral("c") && R(plan, 4).depth == 0 && R(plan, 4).placed);
		CHECK(R(plan, 5).uuid == QStringLiteral("free") && !R(plan, 5).placed && R(plan, 5).path.empty());
	}
	{ // 只有主画布时内容恒从 depth 0 起，没有画布分组表头。
		// ObsBridge::liveCanvases 的契约就是只报主画布（副画布不进树），这里锁死投影侧的对应表现：
		// 不因画布这个概念多出任何一行、也不多缩进一级。
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "A"));
		CHECK(s.placeScene("cv1", "m", {0}, 0));
		std::vector<LiveCanvas> live{
			{QStringLiteral("cv1"),
			 QStringLiteral("主"),
			 {{QStringLiteral("m"), QStringLiteral("M")}, {QStringLiteral("free"), QStringLiteral("F")}}}};
		auto plan = planProjection(s, live);
		CHECK(plan.size() == 3);
		CHECK(R(plan, 0).kind == RowPlan::Folder && R(plan, 0).depth == 0);
		CHECK(R(plan, 1).uuid == QStringLiteral("m") && R(plan, 1).depth == 1);
		CHECK(R(plan, 2).uuid == QStringLiteral("free") && R(plan, 2).depth == 0);
	}
	{ // foreign → 全部平铺尾区
		TreeStore s;
		s.fromJson(QStringLiteral("{\"version\":9}"));
		std::vector<LiveCanvas> live{
			{QStringLiteral("cv1"), QStringLiteral("主"), {{QStringLiteral("a"), QStringLiteral("A")}}}};
		auto plan = planProjection(s, live);
		CHECK(plan.size() == 1 && !R(plan, 0).placed);
	}
	{ // 同一 canvas 树内重复 uuid（畸形持久化数据）只渲染一行
		TreeStore s;
		s.fromJson(QStringLiteral(
			"{\"version\":1,\"canvases\":{\"cv1\":{\"tree\":["
			"{\"t\":\"scene\",\"uuid\":\"dup\"},"
			"{\"t\":\"folder\",\"name\":\"F\",\"children\":[{\"t\":\"scene\",\"uuid\":\"dup\"}]}"
			"]}}}"));
		std::vector<LiveCanvas> live{{QStringLiteral("cv1"),
					      QStringLiteral("主"),
					      {{QStringLiteral("dup"), QStringLiteral("重复场景")}}}};
		auto plan = planProjection(s, live);
		int sceneRows = 0;
		for (const auto &r : plan)
			if (r.kind == RowPlan::Scene)
				++sceneRows;
		CHECK(sceneRows == 1);
	}
}

static void test_name_fallback()
{
	{ // 复制场景集合场景：uuid 全变，名字不变 → 树必须完整幸存并自愈到新 uuid
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "开场"));
		CHECK(s.placeScene("cv1", "old-a", {0}, 0));
		CHECK(s.placeScene("cv1", "old-b", {}, 1));
		s.stampSceneNames({{"old-a", "待机"}, {"old-b", "结束"}});

		// 模拟 OBS 复制集合：同名场景、全新 uuid
		s.resolveAndPrune({{"cv1", "主", {{"new-a", "待机"}, {"new-b", "结束"}}}});

		CHECK(N(s.nodeAt("cv1", {0})).name == QStringLiteral("开场"));     // 文件夹还在
		CHECK(N(s.nodeAt("cv1", {0, 0})).uuid == QStringLiteral("new-a")); // 已自愈
		CHECK(N(s.nodeAt("cv1", {1})).uuid == QStringLiteral("new-b"));
		CHECK(s.findScene("cv1", "old-a") == std::nullopt); // 旧 uuid 不再存在
	}
	{ // 改名场景：uuid 存活 → 走主键路径，绝不因名字不符而误判
		TreeStore s;
		CHECK(s.placeScene("cv1", "u1", {}, 0));
		s.stampSceneNames({{"u1", "旧名"}});
		s.resolveAndPrune({{"cv1", "主", {{"u1", "新名"}}}}); // 名字变了，uuid 没变
		CHECK(s.findScene("cv1", "u1").has_value());          // 仍在原位
	}
	{ // 真删除：uuid 和名字都没了 → 清除
		TreeStore s;
		CHECK(s.placeScene("cv1", "gone", {}, 0));
		s.stampSceneNames({{"gone", "已删"}});
		s.resolveAndPrune({{"cv1", "主", {}}});
		CHECK(s.findScene("cv1", "gone") == std::nullopt);
	}
	{ // 名字回退不得抢占已被 uuid 命中的场景（避免两个节点解析到同一 uuid）
		TreeStore s;
		CHECK(s.placeScene("cv1", "live", {}, 0));
		CHECK(s.placeScene("cv1", "dead", {}, 1));
		s.stampSceneNames({{"live", "A"}, {"dead", "A"}}); // 两节点同名（异常数据）
		s.resolveAndPrune({{"cv1", "主", {{"live", "A"}}}});
		CHECK(s.findScene("cv1", "live").has_value());
		CHECK(C(s.canvasRoot("cv1")).size() == 1); // dead 被清除而非重复解析
	}
	{ // stampSceneNames 只碰场景节点，不动文件夹名
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "文件夹"));
		CHECK(s.placeScene("cv1", "u", {0}, 0));
		s.stampSceneNames({{"u", "场景名"}});
		CHECK(N(s.nodeAt("cv1", {0})).name == QStringLiteral("文件夹"));
		CHECK(N(s.nodeAt("cv1", {0, 0})).name == QStringLiteral("场景名"));
	}
	{ // 序列化往返带上 name；缺 name 的旧数据仍可解析（向后兼容）
		TreeStore s;
		CHECK(s.placeScene("cv1", "u", {}, 0));
		s.stampSceneNames({{"u", "N"}});
		TreeStore t;
		CHECK(t.fromJson(s.toJson()));
		CHECK(N(t.nodeAt("cv1", {0})).name == QStringLiteral("N"));
		TreeStore o;
		CHECK(o.fromJson(QStringLiteral(
			"{\"version\":1,\"canvases\":{\"cv1\":{\"tree\":[{\"t\":\"scene\",\"uuid\":\"x\"}]}}}")));
		CHECK(o.findScene("cv1", "x").has_value()); // 旧格式无 name 不报错
		CHECK(N(o.nodeAt("cv1", {0})).name.isEmpty());
	}
	{ // 顺序无关：uuid 已死且同名的节点排在前面，不得抢走后面节点正当持有的场景
		TreeStore s;
		CHECK(s.placeScene("cv1", "stale-ghost", {}, 0));
		CHECK(s.placeScene("cv1", "genuine-live", {}, 1));
		s.stampSceneNames({{"stale-ghost", "Shared"}, {"genuine-live", "Shared"}});
		s.resolveAndPrune({{"cv1", "主", {{"genuine-live", "Shared"}}}});
		CHECK(C(s.canvasRoot("cv1")).size() == 1); // 幽灵已清除
		CHECK(N(s.nodeAt("cv1", {0})).uuid == QStringLiteral("genuine-live"));
	}
	{ // 跨画布不得盗用：cv1 的僵尸不能认领 cv2 的活跃场景
		TreeStore s;
		CHECK(s.placeScene("cv1", "cv1-dead", {}, 0));
		CHECK(s.placeScene("cv2", "cv2-live", {}, 0));
		s.stampSceneNames({{"cv1-dead", "X"}, {"cv2-live", "X"}});
		s.resolveAndPrune({{"cv1", "主", {}}, {"cv2", "竖屏", {{"cv2-live", "X"}}}});
		CHECK(C(s.canvasRoot("cv1")).empty()); // cv1 的僵尸被清除，未偷 cv2 的
		CHECK(N(s.nodeAt("cv2", {0})).uuid == QStringLiteral("cv2-live"));
	}
	{ // 跨画布移动的场景不得在原画布留幽灵
		TreeStore s;
		CHECK(s.placeScene("cv1", "moved", {}, 0));
		s.stampSceneNames({{"moved", "M"}});
		s.resolveAndPrune({{"cv1", "主", {}}, {"cv2", "竖屏", {{"moved", "M"}}}});
		CHECK(C(s.canvasRoot("cv1")).empty());
	}
	{ // 旧数据无名字 + uuid 已死 → 清除，不得因空名字匹配到任何东西
		TreeStore s;
		CHECK(s.fromJson(QStringLiteral(
			"{\"version\":1,\"canvases\":{\"cv1\":{\"tree\":[{\"t\":\"scene\",\"uuid\":\"dead\"}]}}}")));
		s.resolveAndPrune({{"cv1", "主", {{"live", ""}}}});
		CHECK(C(s.canvasRoot("cv1")).empty());
	}
	{ // 空活跃集不可能合法 —— 必须原样不动，绝不清空
		TreeStore s;
		CHECK(s.insertFolder("cv1", {}, 0, "F"));
		CHECK(s.placeScene("cv1", "u", {0}, 0));
		const QString before = s.toJson();
		s.resolveAndPrune({});
		CHECK(s.toJson() == before);
	}
	{ // 旧版本写过的画布折叠态 co["expanded"]：标题行已移除，读到必须忽略且不影响树内容
		TreeStore t;
		CHECK(t.fromJson(QStringLiteral("{\"version\":1,\"canvases\":{\"cv1\":{\"expanded\":false,"
						"\"tree\":[{\"t\":\"scene\",\"uuid\":\"u\"}]}},\"mru\":[]}")));
		CHECK(N(t.nodeAt("cv1", {0})).uuid == QStringLiteral("u"));
		CHECK(!t.toJson().contains(QStringLiteral("expanded\":false"))); // 不再写回
	}
}

int main()
{
	test_roundtrip();
	test_bad_json();
	test_foreign_version();
	test_failed_op_leaves_store_untouched();
	test_mutators();
	test_move();
	test_prune();
	test_edge_cases();
	test_name_fallback();
	test_projection();
	if (failures) {
		std::printf("%d FAILURES\n", failures);
		return 1;
	}
	std::printf("all ok\n");
	return 0;
}
