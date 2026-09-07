[CmdletBinding()]
param(
    [string]$GameBin = 'D:\Steam Library\steamapps\common\Crimson Desert\bin64',
    [string]$Branch = 'feature/wardrobe-foundation',
    [string]$BackupRoot = '',
    [switch]$BuildOnly,
    [switch]$SkipBackup
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Step([string]$Message) {
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Fail([string]$Message) {
    throw "FREQEE Wardrobe build/deploy: $Message"
}

function Run-Git {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Args)
    & git @Args
    if ($LASTEXITCODE -ne 0) {
        Fail "git $($Args -join ' ') failed with exit code $LASTEXITCODE"
    }
}

$ProjectDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$RepoRoot = (Resolve-Path (Join-Path $ProjectDir '..')).Path
if (-not $BackupRoot) {
    $BackupRoot = Join-Path $RepoRoot '_runtime_backups'
}

if (-not (Test-Path (Join-Path $RepoRoot '.git'))) { Fail "Repository root not found: $RepoRoot" }
if (-not (Test-Path (Join-Path $ProjectDir 'CMakeLists.txt'))) { Fail "Project directory is invalid: $ProjectDir" }
if (-not (Test-Path $GameBin)) { Fail "Game bin64 directory not found: $GameBin" }
if (-not (Test-Path (Join-Path $GameBin 'CrimsonDesert.exe'))) { Fail "CrimsonDesert.exe not found under: $GameBin" }

Step 'Updating the Wardrobe branch'
Push-Location $RepoRoot
try {
    $dirty = (& git status --porcelain)
    if ($LASTEXITCODE -ne 0) { Fail 'git status failed' }
    if ($dirty) {
        Write-Host 'Tracked/untracked local changes:' -ForegroundColor Yellow
        $dirty | ForEach-Object { Write-Host "  $_" }
        Fail 'Working tree is not clean. Refusing to pull/build a mixed source tree.'
    }

    Run-Git fetch --prune origin

    $current = (& git branch --show-current).Trim()
    if ($LASTEXITCODE -ne 0) { Fail 'git branch query failed' }
    if ($current -ne $Branch) {
        Run-Git switch $Branch
    }

    Run-Git pull --ff-only origin $Branch
    Run-Git submodule sync --recursive
    Run-Git submodule update --init --recursive

    $Head = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { Fail 'git rev-parse failed' }
    Write-Host "Branch : $Branch"
    Write-Host "Commit : $Head"
}
finally {
    Pop-Location
}

Step 'Selecting the validated VS2022 bundled CMake toolchain'

# Phase Two established that global CMake 4.4.3 + newer MSVC toolchains can produce duplicate-ImGui linker errors.
# For Wardrobe runtime candidates we intentionally use the VS2022-bundled CMake/toolset that produced the known-good
# baseline and matches the repository's successful windows-2022 CI environment. Do not silently fall back to VS2026.
$vs2022Candidates = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
)

$VsRoot = $null
$CMake = $null
foreach ($candidate in $vs2022Candidates) {
    $candidateCMake = Join-Path $candidate 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $candidateCMake) {
        $VsRoot = $candidate
        $CMake = $candidateCMake
        break
    }
}

if (-not $CMake) {
    Fail 'Validated Visual Studio 2022 bundled CMake was not found. This test build deliberately will not fall back to global CMake 4.x / Visual Studio 2026.'
}

$versionLine = (& $CMake --version | Select-Object -First 1)
if ($LASTEXITCODE -ne 0) { Fail 'VS2022 bundled cmake --version failed' }
if ($versionLine -notmatch 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)') {
    Fail "Could not parse CMake version from '$versionLine'"
}
$cmakeVersion = [version]$Matches[1]
if ($cmakeVersion -lt [version]'3.28.0') {
    Fail "VS2022 bundled CMake 3.28+ is required; found $cmakeVersion"
}

$Generator = 'Visual Studio 17 2022'
$BuildDir = Join-Path $ProjectDir 'build\wardrobe-v2-vs2022'

Write-Host 'Toolchain: validated VS2022 bundled CMake' -ForegroundColor Green
Write-Host "VS root  : $VsRoot"
Write-Host "CMake    : $CMake"
Write-Host "Version  : $cmakeVersion"
Write-Host "Generator: $Generator"
Write-Host "Build dir: $BuildDir"

# CMAKE_GENERATOR_INSTANCE is deliberately not supplied. The VS17 generator already restricts selection to
# Visual Studio 2022 and is more reliable when CMake resolves the registered installation itself. A previous
# explicit path override can also be cached after a failed configure, so always start this runtime test clean.
if (Test-Path $BuildDir) {
    Step 'Clearing stale VS2022 build directory'
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

Step 'Configuring Wardrobe v2'
& $CMake -S $ProjectDir -B $BuildDir -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed with exit code $LASTEXITCODE" }

Step 'Building Release ASI'
& $CMake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { Fail "CMake build failed with exit code $LASTEXITCODE" }

$BuiltAsi = Join-Path $BuildDir 'CrimsonDesertLiveTransmog.asi'
if (-not (Test-Path $BuiltAsi)) { Fail "Build reported success but ASI is missing: $BuiltAsi" }

$builtHash = (Get-FileHash -Algorithm SHA256 $BuiltAsi).Hash
$builtSize = (Get-Item $BuiltAsi).Length
Write-Host "`nBuilt ASI : $BuiltAsi" -ForegroundColor Green
Write-Host "Size      : $builtSize bytes"
Write-Host "SHA-256   : $builtHash"

if ($BuildOnly) {
    Step 'BuildOnly requested; deployment skipped'
    exit 0
}

if (Get-Process -Name 'CrimsonDesert' -ErrorAction SilentlyContinue) {
    Fail 'CrimsonDesert.exe is running. Close the game before replacing runtime files.'
}

$runtimeNames = @(
    'CrimsonDesertLiveTransmog.asi',
    'CrimsonDesertLiveTransmog.ini',
    'CrimsonDesertLiveTransmog_display_names.tsv',
    'CrimsonDesertLiveTransmog_presets.json',
    'CrimsonDesertLiveTransmog_discovered.json',
    'CrimsonDesertLiveTransmog_discovered_v2.json',
    'CrimsonDesertLiveTransmog.log'
)

if (-not $SkipBackup) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backupDir = Join-Path $BackupRoot "wardrobe-v2-$stamp"
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null

    Step "Backing up current runtime files to $backupDir"
    foreach ($name in $runtimeNames) {
        $source = Join-Path $GameBin $name
        if (Test-Path $source) {
            Copy-Item -LiteralPath $source -Destination $backupDir -Force
        }
    }
    Set-Content -LiteralPath (Join-Path $backupDir 'BUILD.txt') -Encoding UTF8 -Value @(
        "Branch=$Branch",
        "Commit=$Head",
        "Generator=$Generator",
        "CMake=$cmakeVersion",
        "VSRoot=$VsRoot",
        "CandidateSHA256=$builtHash"
    )
}

$DeployedAsi = Join-Path $GameBin 'CrimsonDesertLiveTransmog.asi'
Step 'Deploying Wardrobe v2 ASI'
Copy-Item -LiteralPath $BuiltAsi -Destination $DeployedAsi -Force

$displayNames = Join-Path $ProjectDir 'build\template\CrimsonDesertLiveTransmog_display_names.tsv'
if (Test-Path $displayNames) {
    Copy-Item -LiteralPath $displayNames -Destination (Join-Path $GameBin 'CrimsonDesertLiveTransmog_display_names.tsv') -Force
}
else {
    Write-Warning 'display_names.tsv template not found; retaining the currently installed copy.'
}

$deployedHash = (Get-FileHash -Algorithm SHA256 $DeployedAsi).Hash
if ($deployedHash -ne $builtHash) {
    Fail "Deployment verification failed. Built hash $builtHash != deployed hash $deployedHash"
}

$LogPath = Join-Path $GameBin 'CrimsonDesertLiveTransmog.log'
if (Test-Path $LogPath) {
    Remove-Item -LiteralPath $LogPath -Force
}

Write-Host "`n============================================================" -ForegroundColor Green
Write-Host 'WARDROBE V2 BUILD + DEPLOY SUCCEEDED' -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host "Branch : $Branch"
Write-Host "Commit : $Head"
Write-Host "ASI    : $DeployedAsi"
Write-Host "SHA256 : $deployedHash"
Write-Host "`nNext: launch Crimson Desert normally, press Home, test the Wardrobe, then send the newly generated CrimsonDesertLiveTransmog.log." -ForegroundColor Green
