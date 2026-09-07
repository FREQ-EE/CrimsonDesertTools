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

Step 'Selecting exact Phase Two validated VS2022 toolchain'

# Phase Two already proved the exact local recipe below on this machine.
# VS2022 is physically present but is not registered with the current Visual Studio Installer/vswhere,
# so CMake requires BOTH the path and an explicit version field in CMAKE_GENERATOR_INSTANCE.
$VsRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$VsVersion = '17.11.35327.3'
$VsInstance = "$VsRoot,version=$VsVersion"
$CMake = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$BuildDir = Join-Path $ProjectDir 'build\release-msvc'

if (-not (Test-Path $CMake)) {
    Fail "Validated VS2022 bundled CMake not found at: $CMake"
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

Write-Host 'Toolchain : exact Phase Two validated recipe' -ForegroundColor Green
Write-Host "VS root   : $VsRoot"
Write-Host "VS version: $VsVersion"
Write-Host "Instance  : $VsInstance"
Write-Host "CMake     : $CMake"
Write-Host "Version   : $cmakeVersion"
Write-Host 'Preset    : msvc-release'
Write-Host "Build dir : $BuildDir"

Step 'Clearing stale Phase Two build directory'
if (Test-Path $BuildDir) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

Step 'Configuring Wardrobe v2 with known-good preset'
Push-Location $ProjectDir
try {
    & $CMake --preset msvc-release -D "CMAKE_GENERATOR_INSTANCE=$VsInstance"
    if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed with exit code $LASTEXITCODE" }

    Step 'Building Release ASI'
    & $CMake --build 'build\release-msvc' --config Release --parallel
    if ($LASTEXITCODE -ne 0) { Fail "CMake build failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
}

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
        "Preset=msvc-release",
        "CMake=$cmakeVersion",
        "VSRoot=$VsRoot",
        "VSVersion=$VsVersion",
        "VSInstance=$VsInstance",
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
