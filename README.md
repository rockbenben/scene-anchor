<img src="assets/logo.svg" width="56" align="left" alt="">

# SceneAnchor

> Folder tree for the OBS 32 scene list — the folders live inside the scene collection, so copying or renaming it never loses them.

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue)](LICENSE) [![365 Open Source Plan #037](https://img.shields.io/badge/365%20Open%20Source%20Plan-%23037-1f6feb)](https://github.com/rockbenben/365opensource)

[⬇ Download for Windows / macOS / Linux](https://github.com/rockbenben/scene-anchor/releases) · [简体中文](README.zh.md)

![The SceneAnchor dock: nested folders, colour labels and unfiled scenes](docs/images/hero-en.webp)

**Scene folders that stay put.** Existing scene-tree plugins keep their folders in a side file keyed by collection name, so copying or renaming a collection loses the lot. SceneAnchor writes the tree into the scene collection's own JSON — the same file as the scenes — so it survives copies, renames, exports, OBS updates, and uninstalling the plugin.

## Supported

| Item | Detail |
|---|---|
| OBS Studio | 32.0 and newer (tested on 32.0.2 and 32.2.2) |
| Platform | Windows x64 — developed and tested here. macOS (Universal) and Ubuntu x86_64 build and package in CI but have not been run on real hardware; see Limitations |
| Scenes | Main canvas only — see Limitations |
| Interface | 12 languages, follows OBS's own language setting |
| Install | Unzip into the OBS plugin directory; macOS installer is unsigned, see Install |

## Features

- **Folder tree** with unlimited nesting for organizing scenes
- **Multi-select drag & drop** to move scenes and folders around, with proper OBS-native undo/redo
- **Live search** that filters the tree as you type — press **Enter** to transition to the first switchable match (folder rows and anything collapsed out of view are skipped, because the walk follows what is actually on screen; in Studio Mode it sets the preview, like every other switch in this dock), **Esc** to clear. Bind a hotkey (Settings → Hotkeys → *SceneAnchor: focus the search box*) and the whole flow is keyboard-only: hotkey, two characters, Enter.
- **Color labels** on folders and scenes — 8 presets plus any custom color, automatically adjusted to stay readable on your theme (see below)
- **MRU strip** of recently-used scenes, one click to switch; how many fit is computed from the dock's width, so names stay readable instead of being chopped to stubs
- **Empty-state guidance**: with no folders yet the dock looks exactly like OBS's own scene list — right down to the left edge, since the expander column is only reserved once a folder exists — so it says how to make the first one; the hint disappears once you do, and also whenever the dock is too small to show it whole (a hint cut off mid-sentence reads as a bug)
- **Options** in the empty-area right-click menu: double-click action (transition / rename / none), whether selecting a scene switches to it, whether to show the MRU strip, whether to show icons (one switch covering folders and scenes alike, the way OBS's own source list does it)
- **Full right-click menu**: transition, transition override, rename, duplicate, copy/paste filters, filters, screenshot, windowed/fullscreen projector, show in multiview, color
- **Tree drawn to stay legible**: the expander chevron and the per-level indent guides are painted by the plugin rather than left to Qt's defaults, so they follow the active theme instead of vanishing on dark ones, and every folder carries a chevron — empty ones included, so a folder never reads as a scene
- **Narrow-dock friendly**: usable down to ~120 logical pixels wide — nothing forces the dock to a minimum width
- **12 languages**: English, Simplified & Traditional Chinese, Spanish, Portuguese (BR), German, Japanese, Russian, French, Korean, Italian, Polish
- Uses only public OBS APIs — no private Qt slots, no `findChild`, no `QMetaObject::invokeMethod` on the main window

### About the color labels

Colors are stored exactly as you pick them, but drawn against the tree's *actual* background: before rendering, the label's HSL lightness is nudged until it clears the WCAG 3:1 contrast floor for graphical elements. Hue and saturation are untouched.

This is why presets can be chosen for looks rather than for contrast, and why a custom colour that would be invisible on your theme (a near-black blue on a dark theme, say) still shows up. One stored value stays correct on light and dark themes alike — no per-theme palette, so the same scene collection never renders different colours on different machines.

## Why another scene tree plugin

Existing scene-tree plugins ([DigitOtter/obs_scene_tree_view](https://github.com/DigitOtter/obs_scene_tree_view), [TheThirdRail/scene-tree-view](https://github.com/TheThirdRail/scene-tree-view)) are useful but have three recurring problems SceneAnchor was built to avoid:

1. **Your folders won't disappear** — the mechanism is in the opening paragraph above. Both linked projects' issue trackers carry reports of exactly the side-file failure it avoids.
2. **Public API only, so it won't silently break.** No private Qt slots, no `findChild`/`QMetaObject::invokeMethod`/`property()` calls into the OBS main window. Plugins that reach into private UI internals tend to break across OBS releases without warning.
3. **Organize, search, and switch — all in one dock.** Nested folders, color labels, and live search together, so you don't need a second plugin just to find a scene in a long list.

## Install

Download the archive for your platform from [Releases](https://github.com/rockbenben/scene-anchor/releases) and extract it into your OBS Studio plugin directory (or use the platform installer if one is provided).

**macOS: the installer is not signed.** Double-clicking the `.pkg` gets you *"cannot be opened because it is from an unidentified developer"* — right-click it and choose **Open** instead, or allow it under System Settings → Privacy & Security. This is about the installer, not the plugin: OBS ships `com.apple.security.cs.disable-library-validation` in its own entitlements (`frontend/cmake/macos/entitlements.plist`) precisely so it can load third-party plugins, so an unsigned plugin loads normally once installed. Signing the installer needs an Apple Developer ID, which most individual OBS plugin authors don't buy — [obs-move-transition](https://github.com/exeldro/obs-move-transition), [waveform](https://github.com/phandasm/waveform) and [obs-multi-rtmp](https://github.com/sorayuki/obs-multi-rtmp) all ship unsigned packages today.

**After installing, enable the dock.** SceneAnchor's dock is not visible by default — OBS does not automatically show new plugin docks. Open OBS's **Docks** menu (View → Docks on some platforms) and check **SceneAnchor** to make it appear. If you don't see the dock after installing, this is almost always the reason — the plugin has not failed to load.

## Limitations

- **Deleting a scene that is nested inside another scene** (i.e. used as a source within another scene): undo restores the deleted scene and its child sources, but does **not** restore its placement entry inside the parent scene that referenced it. OBS's own delete path handles this case with `scene_used_in_other_scenes` + `RemoveSceneAndReleaseNested`; SceneAnchor does not implement an equivalent. This is an accepted, documented gap, not an oversight.
- **Main canvas only.** Scenes belonging to a secondary canvas — a vertical canvas from Aitum Vertical, or any canvas another plugin creates with `obs_frontend_add_canvas` — do not appear in this dock at all. They are switched in that canvas's own dock, which is also where they belong.

  This is a decision, not a missing feature. OBS 32 has no API for setting a secondary canvas's current scene: the frontend surface is `get_canvases` / `add_canvas` / `remove_canvas` and nothing more. The one function that looks generic, `obs_canvas_set_channel(canvas, 0, scene)`, is never called by OBS's own frontend — libobs uses it only inside `obs_set_output_source`, for the main canvas — and plugins that own a canvas typically keep a *transition* source in channel 0, not a scene. Writing a scene there would evict the transition, hard-cut the picture, and leave that plugin's idea of its current scene out of sync with what is on screen. A "generic" implementation would be active breakage.

  Individual plugins do expose their own switching hooks (Aitum Vertical ≥1.6.1 registers `aitum_vertical_switch_scene` on the global proc handler). Supporting one vendor and not the next is neither fair nor maintainable, and a scene this dock cannot switch to earns only two of its three verbs — find, organize, switch. Two is not enough to justify a section of the UI.
- **Toggling "Show in multiview" does not refresh a currently-open multiview projector immediately.** The change takes effect the next time the tree rebuilds (there is no public API to force a multiview refresh).
- **Drag-and-drop is disabled while a search filter is active**, since the filter hides some items and a drop position could no longer match what's visually shown. Clear the search box to re-enable dragging.

- **Selecting a scene switches to it, by default** — including when you move the selection with the arrow keys. This matches OBS's own scene list, but that list is only ever used for switching, while this dock is also used for dragging, renaming and colouring. Arrow-keying past eight scenes to reach a folder is eight live program switches. Turn off **Selection Switches Scene** in the empty-area right-click menu while you reorganize; double-click and the menu's **Transition to Scene** still work.
- **Changing a default in a new build does not reach existing users.** OBS persists plugin settings on exit, so once the plugin has run once, the stored value wins over any default the code declares. Defaults only apply to a fresh install.

**Gray areas**: transition override is stored under the `"transition"` / `"transition_duration"` private-settings keys, and multiview visibility under `"show_in_multiview"`. These keys are accessible through public API calls, but the keys themselves are an OBS frontend convention rather than a documented API contract — a future OBS version could change them without notice.

- **Only Windows has been exercised on real hardware.** Every manual check in [`docs/pre-release-checklist.md`](docs/pre-release-checklist.md) — drag, undo/redo, delete-undo, themes, localisation — was run on Windows against OBS 32.0.2 and 32.2.2. The macOS and Linux targets compile and package in CI, and the plugin uses no platform-specific code, but nobody has yet launched OBS with it on either. Treat those two as untested rather than broken, and please report what you find.

**Cosmetic, unexplained**: on OBS 32.2 (Qt 6.11) the log carries one `QWidgetWindow(… name="scene_anchor_dockWindow") must be a top level window.` warning when the dock is dragged. It does not appear on OBS 32.0 (Qt 6.8). Nothing in this plugin creates or manipulates a `QWindow` — the dock widget is wrapped by OBS's own `OBSDock` — and the dock behaves correctly. Recorded here rather than left unmentioned.

## Reporting bugs

Bugs go to [GitHub Issues](https://github.com/rockbenben/scene-anchor/issues). Four things make a report actionable:

- **OBS version, platform, and plugin version.** The plugin prints the last one when it loads: `[scene-anchor] plugin loaded successfully (version 1.0.0)`.
- **The log from that session.** Help → Log Files → Upload Current Log File gives a shareable link; every line this plugin writes is prefixed `[scene-anchor]`.
- **What you did, what you expected, what happened** — and whether undo or redo was involved, because that is where this plugin's riskiest code paths are.
- **If folders went missing, attach the scene collection itself** (Scene Collection → Export Scene Collection). The tree is stored inside that file, so it is the evidence; a lost-folders report without it usually can't be diagnosed.

If OBS crashed rather than misbehaved, add the crash report too: Help → Crash Reports → Show Crash Reports.

## Translations

Every string lives in `data/locale/<lang>.ini`; the plugin ships 12 of the 77 locales OBS itself supports. OBS falls back to `en-US` for any key a locale is missing, so a partial file is safe to contribute.

Terminology is anchored to OBS's own locale files rather than translated freshly: *scene*, *rename*, *duplicate*, *filters*, *remove*, *transition*, *transition override*, *show in multiview*, *screenshot*, *custom color* and *fullscreen* all reuse the wording OBS already uses in that language, so the plugin reads as part of the host application. Menu items follow OBS's capitalisation too — Title Case in English (`Copy Filters`, `Show in Multiview`), sentence case wherever OBS itself uses it. Two words OBS has no term for — *folder* and *search* — use the platform-conventional word for each language.

Two deliberate divergences. **Dissolve folder** does not borrow OBS's *Ungroup* (`Basic.Main.Ungroup`) even though the operation is identical — a folder is not a group, and reusing the group vocabulary for it would suggest the two features are the same thing. Chinese *Duplicate* stays **创建副本** rather than OBS's 复制. OBS uses 复制 for both *Copy* and *Duplicate*, and this plugin's context menu contains 复制滤镜 (*Copy Filters*) a few rows away — anchoring here would create a real ambiguity rather than remove one.

**Only `en-US`, `zh-CN` and `zh-TW` have been checked by a speaker.** The other nine are term-anchored but unreviewed; corrections are welcome and are a one-line pull request. If you spot a wrong word, that file is the only thing you need to touch.

Keys must match `en-US` exactly — same key set, same `%1` placeholders, UTF-8 without BOM. Run `python tools/check-locales.py` after editing: it checks all of that plus duplicate keys, line endings, empty values, ellipsis consistency, strings long enough to be cut off in a narrow dock, whether the language is one OBS actually ships, and whether every key referenced by `obs_module_text` is defined and every defined key is used.

## Build

Requires the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) toolchain (CMake presets, `buildspec.json`-pinned obs-deps/Qt6). Only OBS Studio 32+ is supported.

**The pinned dependencies are deliberately the oldest supported target** (obs-studio 32.0.2, Qt 6.8.3), not the newest. Qt's binary compatibility runs one way: a plugin built against 6.8 loads on 6.9/6.10/6.11 runtimes, but one built against 6.11 will not load on 6.8. OBS 32.2 ships Qt 6.11; building against it would silently drop support for every OBS 32.0 and 32.1 user. Same reasoning for libobs — compiling against the 32.0.2 headers guarantees we only call APIs that exist across the whole supported range. Only raise the pins if a feature genuinely requires a newer API.

```bash
# Windows x64
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo

# macOS
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo

# Ubuntu x86_64
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --config RelWithDebInfo
```

Unit tests (no OBS runtime required, links libobs directly):

```bash
cmake -S tests -B build_tests -G Ninja
cmake --build build_tests
ctest --test-dir build_tests --output-on-failure
```

## Design notes

[`docs/design.md`](docs/design.md) is the design spec: why the tree is stored inside the collection JSON, why nothing touches OBS's private Qt internals, how the undo model works, and which tradeoffs were made deliberately — including the ones this README only states as facts, such as the icons toggle covering folders and the chevron being painted rather than left to Qt. Source comments cite its sections (`spec §2`, `spec §4`) rather than repeating the reasoning.

[`docs/pre-release-checklist.md`](docs/pre-release-checklist.md) is the manual verification pass, with the measurements each item produced. Run it before shipping a binary — especially after touching the delete, undo or drag paths, which is where its only real defect was caught.

## About the 365 Open Source Plan

Project **#037** of the [365 Open Source Plan](https://github.com/rockbenben/365opensource) — one person + AI, 300+ open-source projects in a year.

[Submit your idea →](https://365.aishort.top/) · [Discord](https://discord.gg/PZTQfJ4GjX) · [Telegram](https://t.me/aishort_top)
