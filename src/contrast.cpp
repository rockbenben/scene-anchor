// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "contrast.h"
#include <algorithm>
#include <cmath>

static double srgbLin(double c)
{
	return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relLum(const QColor &c)
{
	return 0.2126 * srgbLin(c.redF()) + 0.7152 * srgbLin(c.greenF()) + 0.0722 * srgbLin(c.blueF());
}

double contrastOf(const QColor &a, const QColor &b)
{
	const double x = relLum(a), y = relLum(b);
	return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

// 为什么要传**一组**背景而不是一个：行上的色有两种落点——普通行落在树底（QPalette::Base），
// 选中行落在高亮条（QPalette::Highlight）上。图标是重建时烘进 pixmap 的，画下去之后行选不
// 选中它不知道，所以必须一次满足两者，取两者里更苛刻的那个解。量具实测：暗色主题红 #d13438
// 对树底 2.98:1（勉强够）、对高亮条 #284CB8 只有 1.55:1（选中那一刻等于没带色）。
// 不为此重新引入自定义 delegate——docs/design.md §6b 记着 delegate 被删的经过（样式表
// padding 对 delegate 无效、坐标假设错了一条把色带画到展开箭头上），这里一次静态求解就够。
//
// 为什么不改成"一组深浅通吃的固定色"：那样的解存在但窗口极窄（真机实测深色背景 #272A33、
// 浅色 #E5E5E5，可行亮度只有 L∈[0.17,0.23]），八个色相被压到同一亮度且必须满饱和，非常难看。
// 而且标签色是**用户数据**，按主题换一套值会让同一个场景集合在不同主题下显示成不同颜色。
// 改在绘制期适配，既保住存储值的唯一性，也顺带让用户自选的任意颜色（深色背景上的深蓝、
// 浅色背景上的浅黄）自动可读。
QColor readableOn(const QColor &c, const QVector<QColor> &bgs, double target)
{
	// 求解在浮点 HSL 上做，落地是 8 位 RGB：四舍五入能把 4.50 变成 4.49。留一点余量，
	// 判据才不会因为一次取整而红。量具实测：去掉这 0.02，暗色主题的红字落在 #f0bcbe，
	// 对高亮条 4.489:1 —— 差 0.011 就是不过。
	const double need = target + 0.02;
	auto worst = [&](const QColor &t) {
		double w = 1e9;
		for (const QColor &b : bgs)
			w = std::min(w, contrastOf(t, b));
		return w;
	};
	if (worst(c) >= need)
		return c;
	float h = 0, s = 0, l = 0, a = 1;
	c.getHslF(&h, &s, &l, &a);
	if (h < 0)
		h = 0; // 无彩色时 Qt 返回 -1，fromHslF 不接受
	// 扫一遍明度：达标的解里取离原明度最近的那个（颜色改动最小）；一个都达不到时取「最不差」
	// 的那个。两套背景方向相反（提亮利于树底不利于高亮条），所以不能像单背景那样二分一个
	// 方向走到底。101 档、每档几次对数运算，重建一行一次，可忽略。
	double bestL = l, bestScore = -1.0;
	for (int i = 0; i <= 100; ++i) {
		const double cand = i / 100.0;
		const double w = worst(QColor::fromHslF(h, s, float(cand), a));
		const double score = w >= need ? 1000.0 - std::fabs(cand - l) : w;
		if (score > bestScore) {
			bestScore = score;
			bestL = cand;
		}
	}
	return QColor::fromHslF(h, s, float(bestL), a);
}
