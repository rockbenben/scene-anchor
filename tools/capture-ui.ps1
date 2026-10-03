# 打磨稿量具的 Windows 运行器：设好 Qt 运行环境与夹具开关，跑一轮渲染矩阵。
# 由 tools/capture-ui.sh 调用，也可以直接跑：
#   powershell -File tools/capture-ui.ps1 -Theme dark -Lang zh-CN -Icons 1 -Widths 320
param(
    [string]$Theme = "dark",
    [string]$Lang = "zh-CN",
    [string]$Icons = "1",
    [string]$Mru = "1",
    [string]$Widths = "200,240,297,320,449,816",
    [string]$Scale = "1.5",
    [string]$Patch = "",
    [string]$Empty = "0",
    [string]$Height = "420",
    [string]$Platform = "windows",
    [string]$TextDump = "0"
)
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$qt = Join-Path $root ".deps\obs-deps-qt6-2025-08-23-x64"
$exe = Join-Path $root "build_tests\Release\dock_render.exe"
if (-not (Test-Path $exe)) { Write-Error "先构建量具：cmake --build build_tests --config Release --target dock_render" }
$env:PATH = (Join-Path $qt "bin") + ";" + (Join-Path $root ".deps\bin\64bit") + ";" + $env:PATH
$env:QT_PLUGIN_PATH = Join-Path $qt "plugins"
$env:QT_QPA_PLATFORM = $Platform
$env:SA_REPO = $root
$env:SA_OUT = Join-Path $root "design-preview\shots"
$env:SA_THEME = $Theme
$env:SA_LANG = $Lang
$env:SA_ICONS = $Icons
$env:SA_MRU = $Mru
$env:SA_WIDTHS = $Widths
$env:SA_SCALE = $Scale
$env:SA_PATCH = $Patch
$env:SA_EMPTY = $Empty
$env:SA_HEIGHT = $Height
$env:SA_TEXTDUMP = $TextDump
New-Item -ItemType Directory -Force -Path $env:SA_OUT | Out-Null
& $exe 2>&1 | ForEach-Object { [Console]::Out.WriteLine($_.ToString()) }
Write-Output "exit=$LASTEXITCODE"
exit $LASTEXITCODE
