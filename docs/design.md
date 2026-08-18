# SceneAnchor 设计规格

日期：2026-08-17
状态：已实现并在 **Windows 真机**的 OBS 32.0.2 / 32.2.2 上验证，含拖拽、撤销/重做、删除撤销三条交互路径（见 docs/pre-release-checklist.md）。macOS 与 Linux 只有 CI 构建，从未真机运行过，见 §8
最后更新：2026-08-19
仓库名与模块名：`scene-anchor`。**刻意不加 `obs-` 前缀**——obs-plugintemplate wiki 明确不建议
模仿 obs-studio 内部模块的命名（`obs-x264`、`obs-filters` 一类），理由是用户难以分辨哪些是
第三方插件、哪些是 OBS 自带功能。名称同样不含 tree；tree/folder 等发现性关键词放仓库描述、
GitHub topics 与论坛资源页标题，不进名字。

> **代码以 `src/` 为准，设计意图以本文件为准。** 开发期的 12 任务实施计划不再随仓库分发：
> 它含有此后被推翻的旧实现（例如颜色标签早期的 `setData(c, Qt::BackgroundRole)` 整行着色），
> 留着只会被当成现状读。

## 1. 定位

OBS 场景文件夹树 dock。与 DigitOtter/obs_scene_tree_view、TheThirdRail/scene-tree-view 同类，三个差异点，全部有据可查：

1. **文件夹不会丢**：树存进场景集合 JSON 的 `modules` 节点，与场景同文件。复制/切换/重命名/导出集合、插件卸载，数据都跟着走。现有插件把树存在自己的 `scene_tree.json`（以集合名+场景名为 key），直接导致其 issue #37/#42/#43（复制/切换集合丢文件夹）。
2. **不会无声地死**：只用公开 API。公开 API 弃用有多版本重叠期和编译警告；现有插件调私有槽（`SLOT(DuplicateSelectedScene())` 等），OBS 30/31/32 各死一次，且 `ScenesReordered` 已静默失效。
3. **整理+查找+切换做全**：OBS 官方 RFP #5076（$2500，2021 年至今未落地）的社区提案收敛于"文件夹+颜色标签+搜索过滤"三件套，现有插件只做了文件夹。

## 2. 范围（一次性全部实现，不分期）

**包含**：文件夹树（无限嵌套）、多选拖拽、搜索实时过滤、颜色标签（8 预设 + 自定义，按背景自适应对比度）、MRU 最近场景条（数量随宽度自适应）、完整右键菜单、双击行为可配、选中是否切换/MRU 显隐/图标显隐三个开关、树操作 undo/redo、场景删除 undo、三平台 CI。

**不做**（决定过，非延期）：
- **副画布场景一概不进树**（2026-08-18 决定，推翻了早先「显示但不可切换」的做法）。OBS 32 没有「设置某画布当前场景」的 API，frontend 层只有 `get_canvases`/`add_canvas`/`remove_canvas`。看似通用的 `obs_canvas_set_channel(cv, 0, scene)` 不可用：OBS 前端自身从不调它（libobs 唯一调用点是 `obs_set_output_source` 内部、仅对主画布，`obs.c:1815`），且建画布的插件通常在 channel 0 上挂**转场源而非场景**（Aitum Vertical 的 `CanvasDock::SwitchScene`），写进去等于顶掉转场槽位、画面硬切、令对方记录的当前场景与实际画面失同步——「通用支持」实为主动破坏。个别厂商确有自己的口子（Aitum ≥1.6.1 的 `aitum_vertical_switch_scene` 全局 proc），但逐厂适配做一不做二既不公平也难维护。判据是这个 dock 的三个动词：找到、整理、切换；切不了的场景只占两个，不足以换一个 UI 分区。副画布场景在它自己的 dock 里切换，那里也是它们该在的地方。README 声明。
- 同步真实顺序回原生 Scenes dock——无公开 API，做了就回到私有依赖老路。
- 从旧插件迁移导入、文件夹级热键、场景缩略图、树导出——未选入。

**已知限制**（README 必须声明）：删除一个**被其他场景嵌套引用**的场景后，撤销会恢复该场景本身及其子源，但**不会恢复它在父场景中的摆放条目**。OBS 自身的删除路径为此额外做了 `scene_used_in_other_scenes` 检测与 `RemoveSceneAndReleaseNested` 处理（`frontend/widgets/OBSBasic_Scenes.cpp:316-364`），本插件未实现该机制。取舍理由：嵌套场景属进阶用法，而补齐它需要枚举全部场景、快照父场景并在撤销时重建其条目，复杂度显著；宁可发布一个写明的缺口，也不发布一个未说明的。

**已知限制**：无。副画布场景不再进树，早先那条「副画布场景被外部改名后显示滞后」随之消失——它的根源是 libobs 只为主画布转发 `source_rename`（`obs-canvas.c:371-395`），而现在树里本来就只有主画布场景。

**已知灰区**（README 声明）：转场覆盖用 `obs_source_get_private_settings` 的 `"transition"`/`"transition_duration"` key，多画面显隐用 `"show_in_multiview"`——API 公开但 key 是前端约定。多画面投影开着时切显隐不能即时刷新（无公开刷新入口），下次重建生效。

## 3. 架构（5 个翻译单元，约 3000 行含头文件）

| 文件 | 职责 |
|---|---|
| `tree_store.{h,cpp}` | 树结构唯一真相。纯数据+操作，不碰 Qt UI。节点：`folder{name,color,expanded,children}` / `scene{uuid,name,color}`（name 为复制集合后的回退解析器，见 §4）。序列化只讲 JSON 字符串（`toJson`/`fromJson`，同时是 undo 载荷）。**整个文件不 include 任何 libobs 头**——`obs_data` 的边界在 `obs_bridge` 里，存盘时才把这个字符串塞进集合的 modules 节点。单元测试因此只链 `Qt6::Core` 就能跑，不需要 OBS |
| `tree_dock.{h,cpp}` | QWidget dock（`obs_frontend_add_dock_by_id`）。搜索框 + MRU 条 + QTreeView + 工具栏。从 TreeStore 全量重建投影到 QStandardItemModel |
| `obs_bridge.{h,cpp}` | 所有 libobs/frontend 调用集中于此：事件回调、save/load 回调、场景操作、undo 注册 |
| `projection.{h,cpp}` | TreeStore 的树 → 扁平行序列（`RowPlan`）。抽出来是为了让「哪些行、什么层级、什么顺序」能脱离 Qt 单独测；dock 只负责把行画出来 |
| `module.cpp` | `obs_module_load/unload` + 全局单例 |

依赖：libobs、obs-frontend-api、Qt6::Widgets。无第三方库。

## 4. 数据模型

存储：场景集合 JSON → `modules` → `"scene_anchor"`，值为**不透明 JSON 字符串**（非嵌套对象）。

**为什么是字符串而非对象**：`obs_data` 的数组只能装对象 —— `libobs/obs-data.c:464-484` 的 `obs_data_add_json_array` 对非对象元素直接 `continue` 丢弃。若让 obs_data 解析我们的 JSON，`"mru": ["uuid1","uuid2"]` 这类标量数组会被**静默清空**（已在真实 OBS 生命周期中实测复现：`[1,2,{"deep":true}]` 存回后变成 `[{"deep":true}]`）。存成字符串则 obs_data 不参与翻译，任何格式零损失，且 §4 的 foreign 护栏「原始串一字不动」由此真正成立。

```jsonc
{ "version": 1,
  "canvases": { "<canvas-uuid>": { "tree": [
      { "t": "folder", "name": "开场", "expanded": true, "color": "#d13438", "children": [
        { "t": "scene", "uuid": "a1b2...", "name": "待机" }        // name = uuid 失效时的回退解析器
      ]}
  ] } },
  "mru": ["uuid1", "uuid2"] }
```

**所有权规则**：
- store 只记录用户主动放置过的条目。未放置的场景 → 重建时按 OBS 原始顺序追加在树底部「未归类」尾区（纯视图层，不写入 store）。拖进文件夹的那一刻才进 store。
- 场景引用**以 UUID 为主键、场景名为回退解析器**（`uuid` + `name` 两个字段都存）。改名因此是零成本事件（uuid 存活，永不走回退）。

  **为什么必须有回退**：OBS 32.0.2 复制场景集合时（`SetupDuplicateSceneCollection`，`frontend/widgets/OBSBasic_SceneCollections.cpp:159-205`）会对 `sources` 数组逐项执行 `obs_data_set_string(data, "uuid", os_generate_uuid())` —— **每个场景都换新 uuid**。而 `modules` blob 随文件整份拷贝、内容不被触碰。若只认 uuid，复制后整棵树的场景全部变僵尸被清除，本文档 §1 差异点 1 的旗舰主张即为假。
  对照：`SetupRenameSceneCollection`（:207-240）只改文件名与 name 字段，uuid 保留；canvases 存于独立的 `"canvases"` 数组（:869/:1175），复制时不重生 —— 故 canvas 键无需回退。

  **为什么名字是可靠回退而非启发式**：OBS 强制 source 名全局唯一，且复制集合时名字逐字保留，映射是精确的。名字仅在 uuid 未命中时使用，不会重新引入竞品那类"改名断链"（竞品败因是以名字为主键、且存在以集合名索引的侧文件里）。

  **解析顺序（必须先解析后清除）**：加载时对每个场景节点，uuid 命中活跃集则直接用；未命中但名字命中则**就地改写为新 uuid（自愈）**；两者都未命中才判为僵尸清除。
- 文件夹按位置存，无唯一名要求（旧插件的全套名字查重机器不存在）。
- **僵尸条目只在 load 时清理，会话内永不清理**：场景被删 → 视图跳过，store 保留 → Ctrl+Z 恢复同 UUID 场景回到原文件夹。
- **版本护栏**：存储 `version` 高于支持版本 → 树按平铺显示，save 时把原始 blob 原样写回不覆盖（防降级毁数据，~15 行）。

**设置分层**：用户偏好 → `obs_frontend_get_user_config()` 的 `[SceneAnchor]` 段；树/颜色/展开态/MRU 列表 → 集合 modules 数据。

目前的偏好项（键名与默认值成对定义在 `tree_dock.cpp` 的 `Opt` 常量里，读取处与菜单勾选处取同一份，避免两边各写一遍默认值而分叉）：

| 键 | 默认 | 含义 |
|---|---|---|
| `DoubleClick` | `transition` | 双击行为：`transition` / `rename` / `none` |
| `SelectSwitches` | `true` | 选中（含方向键移动选中）是否切换场景 |
| `ShowMru` | `true` | 是否显示最近使用条 |
| `SceneIcons` | `true` | 是否显示行图标，**文件夹与场景一起管**（键名是第一版留下的，改名会丢用户已落盘的设置）|

**注意**：OBS 退出时会把这些值落盘，因此**改代码里的默认值对已运行过本插件的用户无效**——默认值只对全新安装生效。

## 5. 重建算法（核心循环）

任何变动（OBS 事件 / 用户操作 / undo）→ 全量重建视图：

1. `obs_frontend_get_canvases` → 每 canvas `obs_canvas_enum_scenes` 得 (uuid, name) 活跃集
2. 走 store 树：folder → 建节点；scene-uuid ∈ 活跃集 → 建节点（名字取实时值）标记已消费；∉ → 跳过（store 保留）
3. 活跃集未消费残余 → 未归类尾区
4. 恢复展开态（store）、选中（按 UUID）、滚动位置；高亮当前场景

O(n)，n ≤ 数百。单 canvas 时不显示 canvas 分组层。自发操作用布尔重入锁挡事件重建（如建场景：create → 拿 uuid → store 插入 → 重建）。

## 6. 交互

| 交互 | 实现 |
|---|---|
| 双击 | 可配：转场（默认）/ 重命名 / 无。Studio 模式单击设预览 |
| 搜索 | `QSortFilterProxyModel` + `setRecursiveFilteringEnabled(true)`；非空全展开，清空恢复 store 展开态 |
| MRU | 监听 `SCENE_CHANGED` 头插去重，上限 5，树上方 chips，点击即切，不参与拖拽。**显示几个由 dock 宽度算出**：先按可读下限求出放得下几个，再让它们均分宽度，放不下的不画——不这么做时五个 chip 会各自缩成「自…屏」这种残段并顶出一条横向滚动条。省略取 ElideMiddle：OBS 场景名普遍带类别前缀，从尾部截会让多个 chip 长得一模一样 |
| 多选拖拽 | `ExtendedSelection`；drop 按序插入（修复旧插件 row 不递增 bug） |
| 颜色 | 8 个预设 + 自定义（QColorDialog）+ 清除，folder/scene 通用。**呈现方式：给该行已有的图标染色**，不新增视觉元素；绘制前按树视图实际背景把明度调到 WCAG 3:1（色相饱和度不动），故存的是用户原色、画的是可读版本。菜单里当前色以描边环标示——带图标的 QAction 其勾选标记会被图标盖掉 |
| 删除场景 | 确认对话框 → `obs_source_remove`，注册 undo |
| 图标 | 自带 SVG（folder/scene/plus/minus 各深浅两套）。**深浅判定不用 `obs_frontend_is_theme_dark()`**：它返回的是解析自主题文件 `dark:` 键的 `OBSTheme::isDark`，System 主题没有该键、成员未初始化，返回值不可信。改问树视图实际画在什么背景上：`view_->palette().color(QPalette::Base).lightness() < 128` |

**右键菜单**（全公开 API）：切换 / 转场覆盖（含时长 SpinBox）/ 重命名 / 复制场景（`obs_scene_duplicate`，新场景插为原节点兄弟）/ 复制粘贴滤镜（`obs_source_copy_filters`）/ 截图（`obs_frontend_take_source_screenshot`）/ 滤镜 / 窗口投影 / 全屏投影（`QGuiApplication::screens` 枚举显示器 → `obs_frontend_open_projector`）/ 多画面显隐 / 颜色子菜单。文件夹项：新建子文件夹 / 重命名 / 颜色 / 解散（子项上移）/ 删除（含场景则确认）。

**Undo/Redo**（`obs_frontend_add_undo_redo_action`）：
- 树操作（移动/建删改文件夹/颜色）：undo_data = 操作前树 JSON 串，回调反序列化+重建，单一路径无特例
- 场景删除：`obs_save_source` 快照场景 + 树 JSON；undo 时 `obs_load_source` 恢复场景并还原树

## 6b. UI 取舍记录（真机实测后补，2026-08-18）

写下来是因为这几条都试错过不止一轮，且失败原因不看现象无法预判。

**颜色标签的呈现，换过三版**
1. `Qt::BackgroundRole` 铺满整行 —— 与 Qt 的选中高亮抢同一个视觉通道，带色行看起来像被选中。
2. 行左侧独立色带 —— 近了与图标粘成一坨、远了飘成孤立一列，中间没有好位置；且为腾出槽位需要两遍绘制。期间误以为 `QTreeView::item { padding-left }` 能让位，**装了自定义 delegate 之后该样式表规则完全不生效**，据此算出的坐标把色带画到了展开箭头上。
3. **给行内已有的图标染色**。不新增元素，因而没有远近问题；饱和色图标夹在一列灰图标里扫视效率更高。`ColorBarDelegate` 及其全部坐标常量随之删除（净减 46 行）。

**为什么不改成一组"深浅通吃"的固定预设色**：解存在但窗口极窄。真机实测树背景 #272A33、浅色主题 #E5E5E5，同时满足 3:1 的亮度区间只有 L∈[0.17,0.23]，八个色相被压到同一亮度且必须满饱和，解出来是 #f80000 / #927900 / #da00da 之流。且标签色是用户数据，按主题换值会让同一集合在不同主题下显示成不同颜色。改为绘制期按实际背景调明度，一份存储值两种主题都成立。

**dock 的最小宽度曾被 MRU 条顶死**：`QHBoxLayout` 把所有 chip 的自然宽度之和当作自己的最小宽度，并原样传导给整个 dock。量具 `tests/ux_probe.cpp` 实测：搜索框 47、树 56、按钮行 153，MRU 条 915，dock 整体 915。修法是把 MRU 装进横向滚动区切断传导，再按宽度自适应 chip 数量与宽度。现最窄可用至约 120 逻辑像素。

**图标开关反复过三次，最终落在「一个开关同时管文件夹与场景，默认开」**。第一版默认关，论证是"文件夹和场景都有图标则图标不说明任何事，关掉后有图标=文件夹才是真信号"，且 OBS 原生场景列表确实零图标——论证成立但**输给排版**：原生列表是平铺的、只有一种节点，而本插件的树在同一层同时放文件夹与场景。第二版改为默认开、但开关只管场景图标，这是错的：QTreeView 的缩进是 `(层级 + 根装饰) * indentation`，补不回缺失的图标栏，真机实测顶层场景名与顶层文件夹名差出整整一个图标宽（41 / 74 @150% DPI），而文件夹**内**的场景名反倒比其父文件夹名更靠左（66 / 74），层级在视觉上是反的。第三版给场景行塞等宽透明占位来对齐——对齐了，但关掉图标仍白占 24 物理像素，既没图标也没省地方。最终照 OBS 自己的做法（`SourceTreeItem.cpp:54-72`，分组图标与其他行图标同属一个 `iconsVisible` 开关，关掉时连 QLabel 都不创建）：一个开关管住两者，默认开。空文件夹的可识别性不再依赖图标——展开箭头由插件自绘，每个文件夹行都有（见下）。

**副画布场景点击无反馈曾是真缺陷**：`currentChanged` 无条件调 `switchToScene`，而该函数内部静默拒绝非主画布场景。真机日志里用户连点十次同两个副画布场景，屏幕上零变化。先修成「降级显示 + tooltip 说明去哪儿切」，2026-08-18 进一步改为**根本不显示**（见 §2）——一行点了没反应，无论怎么标注都是在为一个不该存在的行找补。`switchToScene`/`transitionToScene` 里的非主画布守卫保留：它挡的是 uuid 在画布间迁移这类竞态，属于入参前置检查，不是死代码。

**展开箭头与缩进导引线由插件自绘，不用 Qt 默认样式**：OBS 的主题文件里一条 `QTreeView` / `::branch` 规则都没有（它自己没有树控件，来源列表是 `QListView` 手工拼 widget），分支指示器因此落到 Qt 默认实现、用 `palette.dark()` 上色——深色主题下实测 rgb(27,27,27) 压 rgb(39,42,51)，对比度 **1.20:1**，同一行的文字是 12.68；而同一个近黑色在浅色主题下是 13.67，说明它根本不随主题变。改由 `AnchorTreeView::drawBranches` 取 `QPalette::Text` 自绘。自绘同时解决第二件事：Qt 只给 `hasChildren` 为真的行画箭头，空文件夹因此没有任何标记；自绘按 `RoleKind` 判断，空文件夹照样有箭头，只是压低透明度（190 → 110）。顺带画一条每层祖先的缩进导引线（透明度 34），因为关掉图标后深度只能靠缩进读。绘制路径里不查模型——`hasChildren` 走构建期缓存的 `RoleHasKids`，避免在 paint 期间让 `QSortFilterProxyModel` 创建内部映射。

**菜单里的当前项标示不能依赖 QAction 勾选**：带图标的 QAction，勾与图标抢同一列，在 OBS 样式表下勾被图标盖掉。颜色子菜单改用描边环；无图标的选项（如「最近使用」「图标」）用勾正常。

## 7. 保存与崩溃安全

- OBS 保存集合 → `on_save` 写 modules；加载 → `on_load` 读，`FINISHED_LOADING`/`SCENE_COLLECTION_CHANGED` 时重建
- 树变动后 2 秒防抖 `obs_frontend_save()`——崩溃最多丢 2 秒动作，原子性由 OBS 保存管线负责

## 8. 平台与 CI

obs-plugintemplate 官方模板。发行产物实测为：Windows x64 `.zip` / macOS universal `.pkg` / Linux `.deb`（另出 `-dbgsym.ddeb` 与一份源码 `source.tar.xz`）——**Linux 没有 tar.gz**，早先这里写的 `deb+tar.gz` 是照抄模板文档、未经核对。`buildspec.json` 锁 obs-deps 与 Qt 版本。仅 OBS 32+。本地开发 Windows，三平台走 GitHub Actions。

**pin 策略：锁最老的受支持目标，不锁最新。** 当前 obs-studio 32.0.2 + Qt 6.8.3。Qt 的二进制兼容是单向的——6.8 编的能在 6.9/6.10/6.11 运行时上加载，6.11 编的无法在 6.8 上加载。OBS 32.2 已换 Qt 6.11，若跟着升 pin，等于静默放弃全部 32.0 与 32.1 用户。libobs 同理：按 32.0.2 头文件编译，保证只调用整个受支持区间都存在的 API。仅当确需 32.1+ 才有的 API 时才抬 pin。已在 OBS 32.0.2 与 32.2.2 上分别验证加载与运行——**两次都在 Windows 上**。macOS 与 Linux 目标能在 CI 上编译打包，插件也没有任何平台相关代码，但至今没有人在这两个系统上把 OBS 带着它启动过；README 的「已知限制」里如实声明了这一点，别在发布材料里把「三平台出包」写成「三平台验证过」。

**插件清单 `manifest.json` 由 CMake 生成，不是静态文件。** OBS 32 的 `obs_module_load_metadata` 会读插件 data 目录下的这个文件；`display_name` 一旦存在就**压过** C 导出的 `obs_module_name()`，所以品牌名在这里定死为英文，界面里那个可本地化的名字由 dock 标题承担。名字、id、版本、网址全部从 `buildspec.json` 取，发版时不会漏改一处。完整取舍写在 `CMakeLists.txt` 生成它的那几行上方，此处不复述。

## 9. 测试

- **单元**：`test_tree_store` 只链 `Qt6::Core`——不链 libobs，也不链 Qt Widgets，因而在任何装了 Qt 的机器上都能跑。覆盖两块——
  - `TreeStore`：序列化往返、load 清僵尸、会话内保僵尸、多选移动顺序、版本护栏原样写回、未归类不入 store
  - `planProjection`：僵尸跳过、未归类追加在尾、名字取实时值、外来版本全平铺、畸形数据里重复 uuid 只渲染一行，以及**内容恒从 depth 0 起**——这一条锁的是「副画布不进树」在投影侧的表现：不因画布这个概念多出任何一行、也不多缩进一级
- **UI 量具** `tests/ux_probe.cpp`（`EXCLUDE_FROM_ALL`，不进 ctest）：离屏构造各部件，量 `minimumSizeHint` 与真实布局结果，用于回答"这个 dock 能被拖到多窄"这类问题。MRU 顶死 dock 最小宽度那个缺陷就是它量出来的
- **手动清单**（发布前）：复制集合 / 切换集合 / 重命名集合 / 强杀 OBS 四杀手场景 + Studio 模式 + 主题明暗切换

## 10. 生存风险声明

若 OBS 原生实现 RFP #5076，全品类插件归零。悬赏挂五年、四提案未落地，窗口期以年计。届时数据已在场景集合内，写导出迁移最容易。

## 11. 发布

### 对外文案

| 字段 | 值 |
|---|---|
| GitHub description | `OBS 32 scene folders that survive collection copies and renames ｜ OBS 32 场景文件夹，复制改名集合都不丢` |
| homepage | `https://github.com/rockbenben/scene-anchor/releases`，论坛资源页上线后换成资源页 |
| topics | `obs-studio` `obs-plugin` `scene-organizer` `scene-tree` `folders` `streaming` `windows` `macos` `linux` |
| 社交预览图 | `assets/social-card.png`（1280×640），Settings → Social preview 上传 |
| 论坛资源页标题 | `SceneAnchor — Scene Folder Tree with Search & Colors` |

四条口径，改文案之前先过一遍：

- **description 必须中英双语且 ≤174 字符**（= 200 硬截断 − ` - rockbenben/scene-anchor`，仓库全名 23 字符）。
  它只有一个字段却要同时接住两种读者；超出的部分连同后半句一起被卡片和列表页截掉，
  所以英文在前——`README.md` 是英文，次要的那半句才是可以被截的。当前 88 字符。
- **topics 用途词与平台词在前，`qt6`/`cpp` 不放**——对使用者零信息量，和首屏不放
  Platform 徽章是同一个道理。
- **社交预览图上传即覆盖且旧图不可找回**，之后不要顺手「优化一下」。它和 README 首屏那张
  `docs/images/hero-*.webp` 不能互相替代：一个是信息流缩略图（要大字大色块），
  一个是给已经点进来的人看的真实界面（要细节）。
- **论坛标题自带「是什么」**，因为论坛列表只有一行、没有仓库名兜底；`description` 旁边
  永远跟着仓库名，可以省掉这次自我介绍。标题里放 Search 与 Colors 而不是「存在集合内」，
  是因为列表里读者比的是功能面，那个差异点要点进去才体会得到。

### 许可证：GPL-2.0-or-later 是义务，不是偏好

`libobs` 的头文件写的是 GPL **v2 or later**，且 obs-studio 全仓库没有任何链接例外
（Qt 那种 LGPL 动态链接豁免，OBS 没有）。插件把 libobs 与 obs-frontend-api 链进同一个
DLL，分发出去的二进制就是衍生作品，只能按 GPL-2.0+ 分发。OBS 论坛的资源政策把这条写成了
准入条件：*"Any proper plugin that links with OBS Studio is subject to the GPL v2.0+ …
This means that a plugin must have the source code available in compliance with the license."*

**MIT 在这里是个陷阱而不是选项**：MIT 与 GPL 兼容，你确实可以给自己的源文件挂 MIT 头，
但分发的 DLL 仍受 GPL 约束——结果是仓库标着 MIT、用户拿到的东西却不是，门面在骗人，
还换不来任何实际自由度。

**声明方式**：我们自己的 11 个源文件用 `SPDX-License-Identifier: GPL-2.0-or-later`
两行头；模板生成的 `plugin-support.h` 保留它自带的完整声明块（语义等价，且改了下次同步
模板还会回来）。`LICENSE` 是 GPLv2 正文——v2 与 v2-or-later 共用同一份文本，「or later」
活在各文件的声明里，所以 **GitHub 侧栏会显示 "GPL-2.0"**，obs-plugintemplate 自己就是
这个状态，不是错。

### Slogan（中英各自成立，围绕 anchor 的字面含义，不直接互译）

| 位置 | 文案 |
|---|---|
| EN | **Scene folders that stay put.** |
| 中 | **文件夹放在哪就留在哪。** |

取这句的理由：它同时命中 §1 的两个差异点（树存在集合内故复制不丢；只用公开 API 故升级不死），
且不用 "anchor" 这个词就把锚的意思说完了。中文没有译成「场景文件夹，放哪就在哪」那种对仗句，
是因为主语换成「文件夹」后更贴近用户实际担心的东西——他担心的不是「场景」跑掉，是自己整理的
那层结构没了。**不写「最强 / 最好用的场景树插件」这类无法证伪的话。**

README 首屏那句一句话（`Folder tree for the OBS 32 scene list — …`）与 slogan 分工不同：
slogan 管气质，一句话管事实与检索词，两者不互相复述。改动其一时回头看另一处是否还成立。
