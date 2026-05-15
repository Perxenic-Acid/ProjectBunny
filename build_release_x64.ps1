# build_release_x64.ps1 — 快捷构建 Release x64
$root = Split-Path -Parent $PSCommandPath
& "$root\scripts\build-cmake.ps1" -Configuration Release -Platform x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
