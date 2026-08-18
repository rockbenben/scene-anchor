// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// UI 量具：测 dock 各部件的最小宽度，回答"这个 dock 能被拖到多窄"。
// 不进构建产物，只在本地跑。用真机上的实际场景名，字体用系统默认（与 OBS 主题字体
// 可能有出入，故绝对值仅供参考，改动前后的相对差值才是结论）。
#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

// 真机 2026-08-18 截图中的 MRU 内容（自习室 场景集合）
static const char *kNames[] = {"解说录制-1080p", "YY开播-去背景|加底图", "屏幕录制", "scrcpy 虚拟摄像头",
			       "自习室-横屏"};

// 当前实现：QToolButton 直接放文字，无省略、无宽度上限
static QWidget *buildMruCurrent()
{
	auto *bar = new QWidget();
	auto *lay = new QHBoxLayout(bar);
	lay->setContentsMargins(4, 0, 4, 0);
	lay->setSpacing(2);
	lay->addStretch();
	int i = 0;
	for (const char *n : kNames) {
		auto *chip = new QToolButton(bar);
		chip->setText(QString::fromUtf8(n));
		chip->setAutoRaise(true);
		lay->insertWidget(i++, chip);
	}
	return bar;
}

// 候选修法：给 chip 设省略文本 + 宽度上限，并让 bar 自身可以被压到很窄
static QWidget *buildMruFixed(int chipMax, QSizePolicy::Policy hp)
{
	auto *bar = new QWidget();
	auto *lay = new QHBoxLayout(bar);
	lay->setContentsMargins(4, 0, 4, 0);
	lay->setSpacing(2);
	lay->addStretch();
	int i = 0;
	for (const char *n : kNames) {
		auto *chip = new QToolButton(bar);
		const QString full = QString::fromUtf8(n);
		chip->setAutoRaise(true);
		chip->setToolTip(full);
		chip->setMaximumWidth(chipMax);
		// 关键：minimumSizeHint 不再跟文字走
		chip->setMinimumWidth(0);
		chip->setSizePolicy(hp, QSizePolicy::Fixed);
		chip->setText(QFontMetrics(chip->font()).elidedText(full, Qt::ElideRight, chipMax - 16));
		lay->insertWidget(i++, chip);
	}
	return bar;
}

// 真实布局结果：把 bar 强制到给定宽度，报告每个 chip 实际占了多少、有没有被挤出边界
static void layoutAt(const char *label, QWidget *bar, int w)
{
	bar->resize(w, 32);
	bar->layout()->activate();
	QString widths;
	int rightMost = 0;
	auto *lay = bar->layout();
	for (int i = 0; i < lay->count(); ++i) {
		if (auto *cw = lay->itemAt(i)->widget()) {
			widths += QString::number(cw->width()) + " ";
			rightMost = qMax(rightMost, cw->x() + cw->width());
		}
	}
	printf("%-30s 容器宽=%3d  chip实宽=[ %s]  最右沿=%3d  %s\n", label, w, widths.toUtf8().constData(), rightMost,
	       rightMost > w ? "← 溢出!" : "ok");
}

static void report(const char *label, QWidget *w)
{
	printf("%-34s minimumSizeHint=%4d  sizeHint=%4d\n", label, w->minimumSizeHint().width(), w->sizeHint().width());
}

int main(int argc, char **argv)
{
	// obs-deps 的 Qt 不带 qoffscreen.dll，只有 qminimal.dll；平台插件缺失时 Qt 会弹模态框卡死。
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", "minimal");
	QApplication app(argc, argv);

	printf("=== 单个部件的最小宽度 ===\n");
	auto *search = new QLineEdit();
	search->setPlaceholderText(QStringLiteral("搜索场景..."));
	search->setClearButtonEnabled(true);
	report("搜索框", search);

	auto *tree = new QTreeView();
	tree->setHeaderHidden(true);
	report("树视图", tree);

	auto *btnRow = new QWidget();
	auto *bl = new QHBoxLayout(btnRow);
	bl->setContentsMargins(4, 0, 4, 4);
	for (const char *t : {"+", "", "-"}) {
		auto *b = new QToolButton(btnRow);
		b->setText(QString::fromUtf8(t));
		bl->addWidget(b);
	}
	bl->addStretch();
	report("底部按钮行", btnRow);

	QWidget *cur = buildMruCurrent();
	report("MRU 条（当前实现）", cur);

	printf("\n=== 整个 dock 的最小宽度 ===\n");
	auto *dockCur = new QWidget();
	auto *dl = new QVBoxLayout(dockCur);
	dl->setContentsMargins(0, 0, 0, 0);
	dl->setSpacing(2);
	dl->addWidget(new QLineEdit(dockCur));
	dl->addWidget(buildMruCurrent());
	dl->addWidget(new QTreeView(dockCur), 1);
	report("dock 整体（当前实现）", dockCur);

	printf("\n=== 实际布局结果：当前实现 ===\n");
	for (int w : {240, 320, 816})
		layoutAt("当前实现", buildMruCurrent(), w);

	printf("\n=== 实际布局结果：Preferred 策略 + 上限 96 ===\n");
	for (int w : {240, 320, 816})
		layoutAt("Preferred", buildMruFixed(96, QSizePolicy::Preferred), w);

	printf("\n=== 实际布局结果：Ignored 策略 + 上限 96 ===\n");
	for (int w : {240, 320, 816})
		layoutAt("Ignored", buildMruFixed(96, QSizePolicy::Ignored), w);

	// 方案 A：塞进横向 QScrollArea —— 最小宽度由滚动区决定，chip 保留全名
	printf("\n=== 方案 A：横向滚动区 ===\n");
	{
		auto *sa = new QScrollArea();
		sa->setWidget(buildMruCurrent());
		sa->setWidgetResizable(true);
		sa->setFrameShape(QFrame::NoFrame);
		sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		sa->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		sa->setFixedHeight(34);
		report("MRU（滚动区）", sa);
	}

	// 方案 B：自适应数量 —— 宽度不够就少显示几个，保留全名可读
	printf("\n=== 方案 B：自适应数量（算得下几个显示几个）===\n");
	{
		QWidget probe;
		QFontMetrics fm(probe.font());
		int natural[5];
		int k = 0;
		for (const char *n : kNames) {
			QToolButton b(&probe);
			b.setText(QString::fromUtf8(n));
			b.setAutoRaise(true);
			natural[k++] = b.sizeHint().width();
		}
		for (int w : {240, 320, 480, 816}) {
			int used = 8, fit = 0; // 8 = 左右各 4 的 margin
			for (int i = 0; i < 5; ++i) {
				const int need = natural[i] + (fit ? 2 : 0);
				if (used + need > w)
					break;
				used += need;
				++fit;
			}
			printf("  容器宽=%3d → 显示 %d/5 个 chip，占用 %d px\n", w, fit, used);
		}
	}

	printf("\n=== 最小宽度对照 ===\n");
	report("MRU（当前）", buildMruCurrent());
	report("MRU（Preferred 上限96）", buildMruFixed(96, QSizePolicy::Preferred));
	report("MRU（Ignored 上限96）", buildMruFixed(96, QSizePolicy::Ignored));

	auto *dockFix = new QWidget();
	auto *dl2 = new QVBoxLayout(dockFix);
	dl2->setContentsMargins(0, 0, 0, 0);
	dl2->setSpacing(2);
	dl2->addWidget(new QLineEdit(dockFix));
	dl2->addWidget(buildMruFixed(96, QSizePolicy::Preferred));
	dl2->addWidget(new QTreeView(dockFix), 1);
	report("dock 整体（修后）", dockFix);

	return 0;
}
