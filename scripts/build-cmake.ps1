# ============================================================
# build-cmake.ps1 — CMake 构建脚本
# 用法: .\scripts\build-cmake.ps1 [-Configuration Debug|Release] [-Platform x64|Win32]
# ============================================================
param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('x64','Win32')]
    [string]$Platform = 'x64'
)

$root = Split-Path -Parent $PSScriptRoot
$cmake = "C:\tools\cmake\cmake-4.3.2-windows-x86_64\bin\cmake.exe"
$buildDir = "$root\build\cmake-$Platform-$($Configuration.ToLower())"

# VS 版本检测
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = if ($Platform -eq 'x64') {
    "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
} else {
    "$vsPath\VC\Auxiliary\Build\vcvars32.bat"
}

# 检查 CMake
if (-not (Test-Path $cmake)) {
    Write-Host "WARN: CMake not found at $cmake" -ForegroundColor Yellow
    # exit 1
    $cmake = "cmake" # 尝试使用系统 PATH 中的 cmake
    if (-not (Get-Command $cmake -ErrorAction SilentlyContinue)) {
        Write-Host "ERROR: CMake not found in system PATH.\nPlease install CMake and ensure it is in your system PATH." -ForegroundColor Red
        exit 1
    } else {
        Write-Host "INFO: Found CMake in system PATH" -ForegroundColor Green
    }
}

# 检查 VS
if (-not (Test-Path $vcvars)) {
    Write-Host "WARN: Visual Studio not found at $vsPath" -ForegroundColor Yellow
    # exit 1
    Write-Host "WARN: Visual Studio not found at $vsPath. Attempting to use vswhere to locate vcvars..." -ForegroundColor Yellow
    $vswherePath = "$env:ProgramFiles(x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswherePath) {
        $vsInstallPath = & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsInstallPath) {
            $vcvars = if ($Platform -eq 'x64') {
                "$vsInstallPath\VC\Auxiliary\Build\vcvars64.bat"
            } else {
                "$vsInstallPath\VC\Auxiliary\Build\vcvars32.bat"
            }
            if (Test-Path $vcvars) {
                Write-Host "INFO: Found Visual Studio vcvars at $vcvars" -ForegroundColor Green
            } else {
                Write-Host "ERROR: Could not find vcvars batch file in Visual Studio installation at $vsInstallPath" -ForegroundColor Red
                exit 1
            }
        } else {
            Write-Host "ERROR: Could not locate Visual Studio installation with required VC tools using vswhere." -ForegroundColor Red
            exit 1
        }
    } else {
        Write-Host "ERROR: vswhere.exe not found at $vswherePath. Cannot locate Visual Studio installation." -ForegroundColor Red
        exit 1
    }
}

# 生成临时批处理文件（避免 cmd /c 的 & 转义问题）
$tmpScript = "$env:TEMP\cmake_build_$PID.bat"
$content = @"
@echo off
call "$vcvars" > nul
if errorlevel 1 exit /b 1
"$cmake" -S "$root" -B "$buildDir" -G "Ninja" -DCMAKE_BUILD_TYPE="$Configuration"
if errorlevel 1 exit /b 1
"$cmake" --build "$buildDir"
"@

# CMake 配置
Write-Host "===== Configuring CMake ($Configuration|$Platform) =====" -ForegroundColor Cyan
$content | Set-Content -Path $tmpScript -Encoding ASCII
cmd /c $tmpScript 2>&1
$exitCode = $LASTEXITCODE
Remove-Item $tmpScript -Force -ErrorAction SilentlyContinue
if ($exitCode -ne 0) {
    Write-Host "FAILED: CMake $($Configuration) $($Platform)" -ForegroundColor Red
    exit $exitCode
}

# 复制产物到统一输出目录
$outDir = if ($Platform -eq 'x64') { "$root\x64\$Configuration" } else { "$root\x32\$Configuration" }
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

$files = @(
    "src/DirectX11/d3d11.dll",
    "src/DirectX12/d3d12.dll",
    "src/D3DCompiler_46/d3dcompiler_47.dll",
    "src/DirectXGI/dxgi.dll",
    "src/Injector/3DMigoto Loader.exe",
    "src/InjectorLib/3dmloader.dll",
    "src/D3D_Shaders/D3D_Shaders.exe"
)
foreach ($f in $files) {
    $src = "$buildDir/$f"
    $dst = "$outDir/$(Split-Path -Leaf $f)"
    if (Test-Path $src) {
        Copy-Item $src $dst -Force
        Write-Host "  Copied: $dst" -ForegroundColor Gray
    }
}

if ($Platform -eq 'x64') {
    $dx12GameRoot = "C:\SteamLibrary\steamapps\common\StellarBladeDemo"
    if (-not (Test-Path $dx12GameRoot)) {
        Write-Host "  Warning: DX12 test game root not found at $dx12GameRoot. Skipping DX12 test DLL copy and launch." -ForegroundColor Yellow
        return
    }
    $dx12TestDir = Join-Path $dx12GameRoot "SB\Binaries\Win64"
    $dx12Exe = Join-Path $dx12GameRoot "SB.exe"
    $dx12Dll = "$outDir\d3d12.dll"
    if ((Test-Path $dx12Dll) -and (Test-Path $dx12Exe) -and (Test-Path $dx12TestDir)) {
        $dx12Processes = Get-Process -Name "SB-Win64-Shipping" -ErrorAction SilentlyContinue
        if ($dx12Processes) {
            Write-Host "  Stopping running DX12 test game before DLL copy..." -ForegroundColor Gray
            $dx12Processes | Stop-Process -Force
            Start-Sleep -Seconds 1
        }

        $dx12CleanupPaths = @(
            "$dx12TestDir\ShaderDumpDX12",
            "$dx12TestDir\d3d12_log.jsonl",
            "$dx12TestDir\d3d12_log.txt",
            "$dx12TestDir\d3d12_hook.log"
        )
        $dx12CleanupPaths += Get-ChildItem -Path $dx12TestDir -Filter "d3d12_log.before_*.jsonl" -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
        $dx12CleanupPaths += Get-ChildItem -Path $dx12TestDir -Filter "d3d12_log.before_*.txt" -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
        foreach ($cleanupPath in $dx12CleanupPaths) {
            if (Test-Path $cleanupPath) {
                try {
                    Remove-Item $cleanupPath -Recurse -Force -ErrorAction Stop
                    Write-Host "  Removed old DX12 test artifact: $cleanupPath" -ForegroundColor Gray
                } catch {
                    Write-Host "  Could not remove old DX12 test artifact: $cleanupPath" -ForegroundColor Yellow
                    Write-Host "  $($_.Exception.Message)" -ForegroundColor Yellow
                }
            }
        }

        Copy-Item $dx12Dll "$dx12TestDir\d3d12.dll" -Force
        Write-Host "  Copied DX12 test DLL: $dx12TestDir\d3d12.dll" -ForegroundColor Gray

        $dx12SteamUri = "steam://run/3564860"
        Start-Process $dx12SteamUri
        Write-Host "  Launched DX12 test game via Steam: $dx12SteamUri" -ForegroundColor Gray
    } elseif (-not (Test-Path $dx12Exe)) {
        Write-Host "  Skipped DX12 test DLL copy: executable not found: $dx12Exe" -ForegroundColor Yellow
    } elseif (-not (Test-Path $dx12TestDir)) {
        Write-Host "  Skipped DX12 test DLL copy: directory not found: $dx12TestDir" -ForegroundColor Yellow
    }
}

Write-Host "`n===== All projects built successfully =====" -ForegroundColor Green
Write-Host "Output directory: $outDir"
