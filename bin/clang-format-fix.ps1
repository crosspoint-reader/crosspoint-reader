<#
.SYNOPSIS
    Runs clang-format -i on project *.cpp and *.h files.

.DESCRIPTION
    Formats all C/C++ source and header files in the repository, excluding
    generated, vendored, and build directories (open-x4-sdk, builtinFonts,
    hyphenation tries, uzlib, .pio, *.generated.h).

    The clang-format binary path is resolved once and cached in
    bin/clang-format-fix.local. On first run it checks a default path,
    then PATH, then common install locations. Edit the .local file to
    override manually.

.PARAMETER g
    Format only git-modified files (git diff --name-only HEAD) instead of
    the full tree.

.PARAMETER h
    Show this help text.

.EXAMPLE
    .\clang-format-fix.ps1
    Format all files.

.EXAMPLE
    .\clang-format-fix.ps1 -g
    Format only git-modified files.
#>

param(
    [switch]$g,
    [switch]$h
)

if ($h) {
    Get-Help $PSCommandPath -Detailed
    return
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$configFile = Join-Path $PSScriptRoot 'clang-format-fix.local'
$defaultPath = 'C:\Program Files\LLVM\bin\clang-format.exe'

$candidatePaths = @(
    'C:\Program Files\LLVM\bin\clang-format.exe'
    'C:\Program Files (x86)\LLVM\bin\clang-format.exe'
    'C:\msys64\ucrt64\bin\clang-format.exe'
    'C:\msys64\mingw64\bin\clang-format.exe'
    "$env:LOCALAPPDATA\LLVM\bin\clang-format.exe"
)

function Find-ClangFormat {
    # Try PATH first
    $inPath = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($inPath) { return $inPath.Source }

    # Try candidate paths
    foreach ($p in $candidatePaths) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Resolve-ClangFormat {
    # 1. Read from config if present
    if (Test-Path $configFile) {
        $saved = (Get-Content $configFile -Raw).Trim()
        if ($saved -and (Test-Path $saved)) { return $saved }
        Write-Host "Configured path no longer valid: $saved"
    }

    # 2. Check default
    if (Test-Path $defaultPath) {
        $defaultPath | Set-Content $configFile
        Write-Host "Saved clang-format path to $configFile"
        return $defaultPath
    }

    # 3. Search PATH and candidate locations
    $found = Find-ClangFormat
    if ($found) {
        $found | Set-Content $configFile
        Write-Host "Found clang-format at $found - saved to $configFile"
        return $found
    }

    Write-Error "clang-format not found. Install LLVM or add clang-format to PATH."
    exit 1
}

$clangFormat = Resolve-ClangFormat

$exclude = @(
    'open-x4-sdk'
    'lib\EpdFont\builtinFonts'
    'lib\Epub\Epub\hyphenation\generated'
    'lib\uzlib'
    '.pio'
    '.venv'
)

function Test-Excluded($fullPath) {
    foreach ($ex in $exclude) {
        if ($fullPath -like "*\$ex\*") { return $true }
    }
    if ($fullPath -like '*.generated.h') { return $true }
    return $false
}

if ($g) {
    # Only git-modified *.cpp / *.h files
    # Covers both staged and unstaged changes
    $files = @(git -C $repoRoot diff --name-only HEAD) +
             @(git -C $repoRoot diff --name-only --cached) |
        Sort-Object -Unique |
        Where-Object { $_ -match '\.(cpp|h)$' } |
        ForEach-Object { Get-Item (Join-Path $repoRoot $_) -ErrorAction SilentlyContinue } |
        Where-Object { $_ -and -not (Test-Excluded $_.FullName) }
} else {
    $files = Get-ChildItem -Path $repoRoot -Recurse -Include *.cpp, *.h -File |
        Where-Object { -not (Test-Excluded $_.FullName) }
}

$files = @($files)

if ($files.Count -eq 0) {
    Write-Host 'No files to format.'
    return
}

Write-Host "Formatting $($files.Count) files..."
$i = 0
$changed = 0
$failures = 0
foreach ($f in $files) {
    $i++
    $rel = $f.FullName.Substring($repoRoot.Length + 1)
    $hashBefore = (Get-FileHash $f.FullName -Algorithm MD5).Hash
    & $clangFormat -i $f.FullName
    if ($LASTEXITCODE -ne 0) {
        $failures++
        Write-Host "  [$i/$($files.Count)] $rel (FAILED, exit code $LASTEXITCODE)"
        continue
    }
    $hashAfter = (Get-FileHash $f.FullName -Algorithm MD5).Hash
    if ($hashBefore -ne $hashAfter) {
        $changed++
        Write-Host "  [$i/$($files.Count)] $rel (changed)"
    } else {
        Write-Host "  [$i/$($files.Count)] $rel"
    }
}
Write-Host "Done. $changed/$($files.Count) files changed, $failures failed."
if ($failures -gt 0) { exit 1 }
