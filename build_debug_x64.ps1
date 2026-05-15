# build_debug_x64.ps1 — 快捷构建 Debug x64
$root = Split-Path -Parent $PSCommandPath
& "$root\scripts\build-cmake.ps1" -Configuration Debug -Platform x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
