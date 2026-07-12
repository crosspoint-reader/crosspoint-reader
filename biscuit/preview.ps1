<#
.SYNOPSIS
    biscuit. e-ink screen preview — hot reload
    
.DESCRIPTION
    Watches test_preview.cpp for changes, auto-builds, auto-opens BMP.
    Just edit test_preview.cpp, save, and the preview appears.

.USAGE
    .\preview.ps1              # watch mode — rebuilds on every save
    .\preview.ps1 -Once        # single build + open
    .\preview.ps1 -Clean       # delete all preview BMPs
#>

param(
    [switch]$Once,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$env:PYTHONUTF8 = "1"

$projectRoot = $PSScriptRoot
if (-not (Test-Path "$projectRoot\platformio.ini")) {
    $projectRoot = (Get-Location).Path
}

$previewFile = "$projectRoot\test\test_preview\test_preview.cpp"
$bmpPattern  = "$projectRoot\test\preview_*.bmp"
$viewer      = $null  # will use default app

# ---- Clean mode ----
if ($Clean) {
    Remove-Item $bmpPattern -Force -ErrorAction SilentlyContinue
    Write-Host "Cleaned all preview BMPs." -ForegroundColor Green
    exit 0
}

# ---- Build + Open ----
function Invoke-Preview {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    
    Write-Host ""
    Write-Host "Building preview..." -ForegroundColor Cyan
    
    $buildOutput = & pio test -e native -f test_preview 2>&1
    $exitCode = $LASTEXITCODE
    $sw.Stop()
    
    if ($exitCode -ne 0) {
        Write-Host "BUILD FAILED ($($sw.ElapsedMilliseconds)ms)" -ForegroundColor Red
        # Show only the first error
        $errors = $buildOutput | Where-Object { $_ -match "error:" } | Select-Object -First 3
        foreach ($e in $errors) {
            Write-Host "  $e" -ForegroundColor Yellow
        }
        return
    }
    
    # Count generated BMPs
    $bmps = Get-ChildItem $bmpPattern -ErrorAction SilentlyContinue
    $count = ($bmps | Measure-Object).Count
    
    Write-Host "OK — $count preview(s) generated in $($sw.ElapsedMilliseconds)ms" -ForegroundColor Green
    
    # List generated files
    foreach ($bmp in $bmps) {
        $size = [math]::Round($bmp.Length / 1024, 1)
        Write-Host "  $($bmp.Name) (${size}KB)" -ForegroundColor DarkGray
    }
    
    # Open the newest BMP (most likely the one being worked on)
    if ($bmps) {
        $newest = $bmps | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        Start-Process $newest.FullName
    }
}

# ---- Single run mode ----
if ($Once) {
    Invoke-Preview
    exit 0
}

# ---- Watch mode ----
if (-not (Test-Path $previewFile)) {
    Write-Host "ERROR: $previewFile not found" -ForegroundColor Red
    Write-Host "Create test/test_preview/test_preview.cpp first." -ForegroundColor Yellow
    exit 1
}

Write-Host @"

  ╔══════════════════════════════════════════════╗
  ║  biscuit. preview — hot reload active        ║
  ║                                              ║
  ║  Edit: test\test_preview\test_preview.cpp    ║
  ║  Save → auto-build → BMP opens              ║
  ║                                              ║
  ║  Press Ctrl+C to stop                        ║
  ╚══════════════════════════════════════════════╝

"@ -ForegroundColor DarkCyan

# Initial build
Invoke-Preview

# Watch for changes
$lastWrite = (Get-Item $previewFile).LastWriteTime
$watchPaths = @(
    $previewFile,
    "$projectRoot\test\mocks\BitmapRenderer.h"
)

Write-Host "`nWatching for changes..." -ForegroundColor DarkGray

while ($true) {
    Start-Sleep -Milliseconds 500
    
    $changed = $false
    foreach ($path in $watchPaths) {
        if (Test-Path $path) {
            $current = (Get-Item $path).LastWriteTime
            if ($current -gt $lastWrite) {
                $lastWrite = $current
                $changed = $true
            }
        }
    }
    
    if ($changed) {
        Write-Host "`n--- file changed at $(Get-Date -Format 'HH:mm:ss') ---" -ForegroundColor DarkYellow
        Invoke-Preview
        Write-Host "Watching for changes..." -ForegroundColor DarkGray
    }
}
