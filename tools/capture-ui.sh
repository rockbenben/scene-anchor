#!/usr/bin/env bash
# 打磨稿取证量具的入口：一次跑完「明暗系统三主题 × 两种语言 × 图标开关 × 宽度」矩阵，
# 把实拍 PNG 与尺寸/文本台账落到 design-preview/shots/。稿子里的「现状」格全部来自这里，
# 不靠手画，也不靠静态复刻 CSS —— 渲染的是真的 src/tree_dock.cpp。
#
#   tools/capture-ui.sh                    # 全矩阵
#   tools/capture-ui.sh --patch gap        # 只重跑一个运行时补丁位，取「改后」的活渲染
#
# 前置：cmake --build build_tests --config Release --target dock_render（本脚本会自己构建）
# 量具不是回归测试：dock_render 是 EXCLUDE_FROM_ALL，不进 ctest、不进发行产物。
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --build build_tests --config Release --target dock_render >/dev/null
rm -f design-preview/shots/report.txt

run() { powershell -NoProfile -ExecutionPolicy Bypass -File tools/capture-ui.ps1 "$@"; }

ARGS=("$@")
if [[ "${1:-}" == "--patch" ]]; then
	run -Patch "${2:-gap}" -TextDump 1
	exit 0
fi

for theme in dark light system; do
	for lang in zh-CN en-US; do
		for icons in 1 0; do
			run -Theme "$theme" -Lang "$lang" -Icons "$icons"
		done
	done
	run -Theme "$theme" -Lang zh-CN -Mru 0        # 关掉最近使用条
	run -Theme "$theme" -Lang zh-CN -Widths 320 -TextDump 1
	# 空库首屏（那条引导提示在窄 dock 与长译文下会不会消失）
	for lang in zh-CN en-US; do
		run -Theme "$theme" -Lang "$lang" -Empty 1 -Widths 200,320
	done
done
echo "实拍与台账落在 design-preview/shots/：report.txt 是尺寸台账，text-metrics.tsv 是文本宽度台账"
