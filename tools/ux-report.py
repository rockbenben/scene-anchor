#!/usr/bin/env python3
"""打磨稿取证台账的汇总：把 design-preview/shots/{report.txt,text-metrics.tsv} 里的原始读数
算成稿子上要写的数字（对比度、放不下的文本、各宽度的 chip 数与条高）。
不参与构建；跑法：python tools/ux-report.py（先跑 tools/capture-ui.sh 取最新读数）
"""
import io, os, re
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "design-preview", "shots")
OUT = os.path.join(SHOTS, "analysis.txt")


def rgb_of(h):
    h = h.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def lum(c):
    def lin(x):
        x /= 255.0
        return x / 12.92 if x <= 0.03928 else ((x + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2])


def lum_from(h):
    return lum(rgb_of(h))


def ratio(h1, h2):
    a, b = lum(rgb_of(h1)), lum(rgb_of(h2))
    hi, lo = max(a, b), min(a, b)
    return (hi + 0.05) / (lo + 0.05)


def hexes(s):
    return re.findall(r"#[0-9a-fA-F]{6}", s)


lines = io.open(os.path.join(SHOTS, "report.txt"), encoding="utf-8").read().split("\n")
out = []

# ── 一、画出来的标签色 vs 背景 ─────────────────────────────────────────────
out.append("=== 一、画出来的标签色 vs 背景（采样自产品真的画进 pixmap 的像素）===")
seen = set()
cell = "?"
for i, ln in enumerate(lines):
    m = re.match(r"^=== 台账 (\S+) ===", ln)
    if m:
        cell = m.group(1)
    if "行颜色采样" not in ln:
        continue
    bg, hl = hexes(ln)[:2]
    for j in range(i + 1, min(i + 40, len(lines))):
        s = lines[j]
        if "行颜色采样" in s or s.startswith("---") or s.startswith("zh-") or s.startswith("en-"):
            break
        if "存=" not in s and "文字色=" not in s:
            continue
        hx = hexes(s)
        if len(hx) < 2:
            continue
        stored, drawn = hx[0], hx[1]
        name = re.search(r"\[(.*?)\]", s)
        if drawn == "#000000":
            continue
        key = (cell, drawn, "文字色" in s)
        if key in seen:
            continue
        seen.add(key)
        tag = "文字色(图标关)" if "文字色" in s else "图标色"
        out.append(f"  {cell:22s} {tag:12s} {name.group(1) if name else '?':26s} "
                   f"存{stored} 画{drawn}  vs底色{bg}={ratio(drawn, bg):.2f}:1  vs选中{hl}={ratio(drawn, hl):.2f}:1")

# ── 二、各宽度实测台账 ─────────────────────────────────────────────────────
out.append("\n=== 二、各宽度实测（逻辑像素）===")
cur = None
cells = []
for ln in lines:
    m = re.match(r"^--- 宽度 (\d+)", ln)
    if m:
        cur = {"w": int(m.group(1)), "rows": []}
        cells.append(cur)
        continue
    if cur is None:
        continue
    for k in ("树", "MRU 条", "空状态提示", "占位符"):
        if k in ln:
            cur[k] = ln.strip()
    if "行场景" in ln or "行文件夹" in ln:
        cur["rows"].append(ln.strip())
for c in cells:
    out.append(f"\n  宽度 {c['w']:>4}   {c.get('树', '')}")
    for k in ("MRU 条", "空状态提示", "占位符"):
        if k in c:
            out.append(f"          {c[k]}")
    for r in c["rows"]:
        if "放不下" in r:
            out.append(f"          {r}")

# ── 三、文本宽度 ───────────────────────────────────────────────────────────
out.append("\n=== 三、文本像素宽（真实字体，按 locale 排序）===")
metrics = defaultdict(dict)
for ln in io.open(os.path.join(SHOTS, "text-metrics.tsv"), encoding="utf-8").read().split("\n")[1:]:
    p = ln.split("\t")
    if len(p) != 4:
        continue
    metrics[p[1]][p[0]] = (int(p[3]), int(p[2]))

CONTAINER = {
    "SceneAnchor.Search": 176,                 # dock 200 时搜索框文字区
    "SceneAnchor.Opt.SelectSwitches": 250,     # 空白区菜单宽度参考
}
for k, per in sorted(metrics.items()):
    if not per:
        continue
    rows = sorted(per.items(), key=lambda t: -t[1][0])
    if k in CONTAINER:
        lim = CONTAINER[k]
        over = [f"{l}={px}px" for l, (px, _) in rows if px > lim]
        out.append(f"  {k:38s} 容器≈{lim}px  超: {', '.join(over) if over else '无'}")
    out.append(f"      {k:38s} 最长 {rows[0][0]}={rows[0][1][0]}px  最短 {rows[-1][0]}={rows[-1][1][0]}px  "
               f"极差 {rows[0][1][0] - rows[-1][1][0]}  ({', '.join(f'{l}:{px}' for l, (px, _) in rows[:4])})")

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print("wrote", OUT, len(out), "lines")
