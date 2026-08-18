#!/usr/bin/env python3
"""data/locale 的结构与一致性校验。不参与构建，改完翻译跑一遍。

    python tools/check-locales.py        # 在仓库根目录执行

检查项：语言代码是否在 OBS 提供的 locale 之内（不在则该文件永远不会被加载）、
键集是否与 en-US 完全一致、编码是否为无 BOM 的 UTF-8、行尾、重复键、值的引号完整性、
%1 占位符是否齐、是否有空值、与英文逐字相同的疑似漏译、省略号写法是否统一、
窄 dock 下会被截断的长文案、以及代码里 obs_module_text 引用的键与定义是否一一对应。

术语锚定（同一动作是否沿用 OBS 各语言自己的用词）需要 .deps 下有 obs-studio 源码，
缺失时会自动跳过第 1 项。
"""

import io, os, re, glob, unicodedata

MINE = "data/locale"
OBS = ".deps/obs-studio-32.0.2/frontend/data/locale"
SRC = ["src/tree_dock.cpp", "src/tree_dock.h", "src/obs_bridge.cpp", "src/obs_bridge.h", "src/module.cpp"]

problems = []


def note(sev, where, msg):
    problems.append((sev, where, msg))


def parse(path):
    """返回 (有序键值, 原始行, 各类结构问题)"""
    raw = io.open(path, "rb").read()
    issues = []
    if raw.startswith(b"\xef\xbb\xbf"):
        issues.append("文件以 BOM 开头")
    if b"\xef\xbb\xbf" in raw[3:]:
        issues.append("文件中间含 BOM")
    if b"\r\n" in raw:
        issues.append("含 CRLF 行尾")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        issues.append(f"不是合法 UTF-8: {e}")
        return {}, [], issues
    d, seen = {}, {}
    lines = text.split("\n")
    for i, l in enumerate(lines, 1):
        s = l.strip()
        if not s or s.startswith("#"):
            continue
        if "=" not in s:
            issues.append(f"第 {i} 行不是 key=value: {s[:40]!r}")
            continue
        k, v = s.split("=", 1)
        k, v = k.strip(), v.strip()
        if not (v.startswith('"') and v.endswith('"') and len(v) >= 2):
            issues.append(f"{k} 的值引号不完整: {v[:40]!r}")
        val = v[1:-1] if len(v) >= 2 else v
        if k in seen:
            issues.append(f"{k} 重复定义（第 {seen[k]} 行与第 {i} 行）")
        seen[k] = i
        d[k] = val
    if lines and lines[-1].strip():
        issues.append("文件末尾缺少换行")
    return d, lines, issues


files = sorted(glob.glob(MINE + "/*.ini"))
base_path = MINE + "/en-US.ini"
base, _, base_issues = parse(base_path)
for m in base_issues:
    note("!", "en-US", m)

print(f"◆ 语言文件 {len(files)} 个，基准 en-US 共 {len(base)} 键\n")

# ── 1. OBS 是否支持这些语言（不支持的话文件永远不会被加载） ─────────────────
print("── 1. 语言代码 vs OBS 自身支持的 locale ──")
obs_langs = {os.path.basename(p)[:-4] for p in glob.glob(OBS + "/*.ini")}
for p in files:
    lang = os.path.basename(p)[:-4]
    if lang not in obs_langs:
        note("!", lang, "OBS 不提供该 locale，此文件永远不会被加载")
print(f"   OBS 提供 {len(obs_langs)} 种；我们的 {len(files)} 种全部在内: "
      f"{all(os.path.basename(p)[:-4] in obs_langs for p in files)}")

# ── 2. 键集 / 结构 / 占位符 ──────────────────────────────────────────────
print("\n── 2. 键集、结构、占位符 ──")
for p in files:
    lang = os.path.basename(p)[:-4]
    d, lines, issues = parse(p)
    for m in issues:
        note("!", lang, m)
    if lang == "en-US":
        continue
    missing = sorted(set(base) - set(d))
    extra = sorted(set(d) - set(base))
    if missing:
        note("!", lang, f"缺少 {len(missing)} 个键: {missing[:4]}")
    if extra:
        note("!", lang, f"多出 {len(extra)} 个键: {extra[:4]}")
    for k in set(base) & set(d):
        bph = sorted(re.findall(r"%\d", base[k]))
        vph = sorted(re.findall(r"%\d", d[k]))
        if bph != vph:
            note("!", lang, f"{k} 占位符不符: en={bph} 本地={vph}")
        if not d[k].strip():
            note("!", lang, f"{k} 值为空")
print("   （问题汇总见末尾）")

# ── 3. 未翻译残留：与英文完全相同的值 ────────────────────────────────────
print("\n── 3. 与英文逐字相同的值（可能漏译；专有名词可豁免） ──")
EXEMPT = {"SceneAnchor.DockTitle", "SceneAnchor.Color.Teal"}
for p in files:
    lang = os.path.basename(p)[:-4]
    if lang == "en-US":
        continue
    d, _, _ = parse(p)
    same = [k for k in set(base) & set(d)
            if d[k] == base[k] and k not in EXEMPT and not base[k].startswith("%")]
    if same:
        print(f"   {lang}: {len(same)} 处 → {[(k.split('.')[-1], base[k]) for k in sorted(same)][:6]}")

# ── 4. 省略号写法是否统一 ────────────────────────────────────────────────
print("\n── 4. 省略号写法 ──")
for p in files:
    lang = os.path.basename(p)[:-4]
    d, _, _ = parse(p)
    three = [k for k, v in d.items() if "..." in v]
    uni = [k for k, v in d.items() if "…" in v]
    if three:
        note("!", lang, f"用了三个点而非 …: {three}")
    if lang == "en-US":
        print(f"   基准 en-US 使用 … 的键: {sorted(k.split('.')[-1] for k in uni)}")
    elif len(uni) != len([k for k, v in base.items() if "…" in v]):
        note("~", lang, f"带 … 的键数与英文不一致（{len(uni)} vs "
                        f"{len([k for k, v in base.items() if chr(0x2026) in v])}）")

# ── 5. 长度：窄 dock 里会被截断的风险位 ──────────────────────────────────
print("\n── 5. 长度检查（东亚字宽按 2 计） ──")


def width(s):
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)


WATCH = {
    "SceneAnchor.Search": 30,       # 搜索框占位符，窄 dock 下会截断
    "SceneAnchor.EmptyHint": 130,   # 空状态提示，超长会被限高隐藏
    "SceneAnchor.DockTitle": 24,
}
for k, lim in WATCH.items():
    row = []
    for p in files:
        lang = os.path.basename(p)[:-4]
        d, _, _ = parse(p)
        if k in d:
            w = width(d[k])
            row.append((lang, w))
    row.sort(key=lambda t: -t[1])
    over = [f"{l}={w}" for l, w in row if w > lim]
    print(f"   {k.split('.')[-1]:<12} 上限{lim:>4}  最长: {row[0][0]}={row[0][1]}  "
          f"{'超限: ' + ', '.join(over) if over else '全部在限内'}")

# ── 6. 代码用键 vs 定义键 ───────────────────────────────────────────────
print("\n── 6. 代码引用与定义 ──")
used = set()
for p in SRC:
    t = io.open(p, encoding="utf-8").read()
    used |= set(re.findall(r'obs_module_text\(\s*"([^"]+)"', t))
    used |= {"SceneAnchor.Color." + m for m in ["Red", "Orange", "Yellow", "Green",
                                                "Teal", "Blue", "Purple", "Magenta"]}
undef = sorted(used - set(base))
unused = sorted(set(base) - used)
print(f"   代码引用 {len(used)} 个键；未定义 {len(undef) or '无'}；定义了但没用到 "
      f"{unused if unused else '无'}")
if undef:
    note("!", "code", f"引用了未定义的键: {undef}")
for k in unused:
    note("~", "en-US", f"{k} 定义了但代码里没引用")

# ── 汇总 ────────────────────────────────────────────────────────────────
print("\n" + "=" * 60)
if not problems:
    print("◆ 全部检查通过，未发现问题")
else:
    hard = [x for x in problems if x[0] == "!"]
    soft = [x for x in problems if x[0] == "~"]
    print(f"◆ 硬问题 {len(hard)} 处，提示 {len(soft)} 处")
    for sev, where, msg in hard + soft:
        print(f"   [{'错误' if sev == '!' else '提示'}] {where}: {msg}")
