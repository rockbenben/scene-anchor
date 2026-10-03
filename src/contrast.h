// Copyright (C) 2026 rockbenben <rockbenben@users.noreply.github.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <QColor>
#include <QVector>

// WCAG 2.1 SC 1.4.11：图形元素对背景至少 3:1；SC 1.4.3：正文至少 4.5:1。
// 两个门槛都要用：同一支标签色，开着图标时是图形（图标染色），关掉图标时是文字。
inline constexpr double kMinContrast = 3.0;
inline constexpr double kMinTextContrast = 4.5;

double relLum(const QColor &c);
double contrastOf(const QColor &a, const QColor &b);

// 把色调到对**每一块它真会落在上的背景**都达标，只动 HSL 明度、保住色相与饱和度。
// 单独成文件是为了能被不链 libobs、也不链 Qt Widgets 的单元测试直接调用 ——
// 与 tree_store/projection 同一个理由。
QColor readableOn(const QColor &c, const QVector<QColor> &bgs, double target);
