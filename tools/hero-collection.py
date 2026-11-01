"""Rebuild the demo collections behind the README first-screen shots.

    python tools/hero-collection.py [输出目录]

默认写进 .deps 里的便携夹具（`.deps/obs33-portable/config/obs-studio/basic/scenes`），
拍图脚本随后用 `--portable --collection Hero|HeroZh` 启动 OBS。这两套集合不在仓库里：
上一版首屏图的演示集合没留下任何配方，界面一改就只能凭截图重搭，所以这份生成器进仓库。
树本身写在集合自己的 `modules.scene_anchor` 里、按场景 uuid 引用，与插件存的格式一致，
启动后不用再手工整理。

两处刻意的安排：文件夹名不能同时当场景名用（会多出不该有的行）；`unfiled` 里的场景
**不进树**，「未归类 · 拖进文件夹」那条表头才会出现——表头只在有未归类尾巴时画。
"""
import io
import json
import os
import sys

MAIN_CANVAS = "6c69626f-6273-4c00-9d88-c5136d61696e"
OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else ".deps/obs33-portable/config/obs-studio/basic/scenes"

# the eight presets from src/tree_dock.cpp; the icons are lightened at draw time
RED, BLUE, YELLOW, PURPLE = "#d13438", "#0078d4", "#c19c00", "#8764b8"

TREES = {
    "Hero": {
        "current": "Face Cam",
        "tree": [
            {"name": "Live", "t": "folder", "color": RED, "expanded": True, "children": [
                {"name": "Starting Soon"},
                {"name": "Main Camera"},
                {"name": "Camera Sources", "t": "folder", "color": BLUE, "expanded": True, "children": [
                    {"name": "Face Cam"},
                    {"name": "Overhead Cam"},
                ]},
                {"name": "Gameplay 1080p60", "color": YELLOW},
            ]},
            {"name": "Recording", "t": "folder", "color": PURPLE, "expanded": True, "children": [
                {"name": "Commentary"},
                {"name": "Screen Capture"},
            ]},
        ],
        # deliberately not in the tree: this is what makes the plugin draw the unfiled header
        "unfiled": ["BRB"],
        "mru": ["Face Cam", "Starting Soon", "Main Camera", "Overhead Cam"],
    },
    "HeroZh": {
        "current": "面部特写",
        "tree": [
            {"name": "直播", "t": "folder", "color": RED, "expanded": True, "children": [
                {"name": "开场等待"},
                {"name": "主机位"},
                {"name": "摄像头素材", "t": "folder", "color": BLUE, "expanded": True, "children": [
                    {"name": "面部特写"},
                    {"name": "俯拍机位"},
                ]},
                {"name": "游戏画面 1080p60", "color": YELLOW},
            ]},
            {"name": "录制", "t": "folder", "color": PURPLE, "expanded": True, "children": [
                {"name": "解说录制"},
                {"name": "屏幕录制"},
            ]},
        ],
        "unfiled": ["稍后回来"],
        "mru": ["面部特写", "游戏画面 1080p60", "稍后回来", "开场等待", "主机位"],
    },
}


def walk(nodes, parent_folder, uuids, out_nodes):
    for node in nodes:
        name = node["name"]
        if node.get("t") == "folder":
            folder = {"name": name, "t": "folder", "expanded": bool(node.get("expanded")), "children": []}
            if node.get("color"):
                folder["color"] = node["color"]
            walk(node.get("children", []), folder, uuids, out_nodes)
            (parent_folder["children"] if parent_folder else out_nodes).append(folder)
        else:
            uuid = uuids[name]
            scene = {"name": name, "t": "scene", "uuid": uuid}
            if node.get("color"):
                scene["color"] = node["color"]
            (parent_folder["children"] if parent_folder else out_nodes).append(scene)


def scene_entry(name, uuid):
    return {
        "prev_ver": 553648129, "name": name, "uuid": uuid, "id": "scene", "versioned_id": "scene",
        "settings": {"id_counter": 1, "custom_size": False, "items": []},
        "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5, "enabled": True,
        "muted": False, "push-to-mute": False, "push-to-mute-delay": 0, "push-to-talk": False,
        "push-to-talk-delay": 0,
        "hotkeys": {"OBSBasic.SelectScene": []},
        "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
        "monitoring_enabled": False, "canvas_uuid": MAIN_CANVAS, "private_settings": {},
    }


def collect_names(nodes, acc):
    """Only scenes become OBS sources; folder names are tree metadata, not scenes."""
    for node in nodes:
        if node.get("t") == "folder":
            collect_names(node.get("children", []), acc)
        else:
            acc.append(node["name"])
    return acc


def build(coll_name, spec):
    names = collect_names(spec["tree"], [])
    for extra in spec.get("unfiled", []):
        names.append(extra)
    uuids = {}
    for i, name in enumerate(names):
        n = 0x5a00 + i * 0x11
        uuids[name] = "%08x-0000-4000-8000-%012x" % (n, n * 0x100000 + i)
    tree = []
    walk(spec["tree"], None, uuids, tree)
    blob = {"canvases": {MAIN_CANVAS: {"tree": tree}},
            "mru": [uuids[n] for n in spec["mru"]], "version": 1}
    doc = {
        "name": coll_name, "version": 2,
        "resolution": {"x": 1920, "y": 1080}, "migration_resolution": {"x": 1920, "y": 1080},
        "sources": [scene_entry(n, uuids[n]) for n in names],
        "groups": [],
        "scene_order": [{"name": n} for n in names],
        "current_scene": spec["current"], "current_program_scene": spec["current"],
        "canvases": [], "current_transition": "Fade", "transition_duration": 300,
        "transitions": [], "quick_transitions": [], "saved_projectors": [],
        "preview_locked": False, "scaling_enabled": False, "scaling_level": -7,
        "scaling_off_x": 0.0, "scaling_off_y": 0.0, "virtual-camera": {"type2": 3},
        "modules": {"scene_anchor": json.dumps(blob, ensure_ascii=False, separators=(",", ":"))},
    }
    path = os.path.join(OUT_DIR, coll_name + ".json")
    with io.open(path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=1)
    print("wrote %s (%d scenes, %d mru)" % (path, len(names), len(spec["mru"])))


if not os.path.isdir(OUT_DIR):
    sys.exit("harness collection dir missing: " + OUT_DIR)
for coll, spec in TREES.items():
    build(coll, spec)
