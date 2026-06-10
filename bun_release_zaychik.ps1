$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSCommandPath
$appDir = Join-Path $root "zaychik"

if (-not (Test-Path (Join-Path $appDir "package.json"))) {
    Write-Host "ERROR: zaychik package.json not found: $appDir" -ForegroundColor Red
    exit 1
}

Push-Location $appDir
try {
    if (-not (Test-Path "node_modules")) {
        bun install
    }

    bun run tauri build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
