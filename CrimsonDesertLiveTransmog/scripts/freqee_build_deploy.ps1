[CmdletBinding()]
param(
    [string]$GameBin = 'D:\Steam Library\steamapps\common\Crimson Desert\bin64',
    [string]$BackupRoot = "$env:USERPROFILE\Documents\Crimson Desert Backups",
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

$ProjectDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$RepoRoot = (Resolve-Path (Join-Path $ProjectDir '..')).Path
$CMake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$VsInstance = 'C:\Program Files\Microsoft Visual Studio\2022\Community,version=17.11.35327.3'
$BuildDir = Join-Path $ProjectDir 'build\release-msvc'
$BuiltAsi = Join-Path $BuildDir 'CrimsonDesertLiveTransmog.asi'
$DeployedAsi = Join-Path $GameBin 'CrimsonDesertLiveTransmog.asi'
$LogPath = Join-Path $GameBin 'CrimsonDesertLiveTransmog.log'
$DiscoveryPath = Join-Path $GameBin 'CrimsonDesertLiveTransmog_discovered.json'

if (-not (Test-Path $CMake)) {
    Fail "Known-good VS-bundled CMake was not found at '$CMake'. Do not substitute global CMake 4.4.3: the validated baseline hit duplicate-ImGui linker errors there."
}
if (-not (Test-Path $ProjectDir)) { Fail "Project directory not found: $ProjectDir" }

Step 'Verifying branch and working tree'
Push-Location $RepoRoot
try {
    $branch = (& git branch --show-current).Trim()
    if ($LASTEXITCODE -ne 0) { Fail 'git branch query failed' }
    if ($branch -ne 'feature/wardrobe-foundation') {
        Fail "Current branch is '$branch'. Switch to 'feature/wardrobe-foundation' before building."
    }

    $dirty = (& git status --porcelain)
    if ($LASTEXITCODE -ne 0) { Fail 'git status failed' }
    if ($dirty) {
        Write-Warning 'Working tree has local changes. The build will include them.'
        $dirty | ForEach-Object { Write-Host "  $_" }
    }

    $head = (& git rev-parse --short=12 HEAD).Trim()
    Write-Host "Branch : $branch"
    Write-Host "Commit : $head"
}
finally {
    Pop-Location
}

Step 'Configuring with the validated VS 2022 / CMake 3.29.5 path'
Push-Location $ProjectDir
try {
    & $CMake --preset msvc-release -D "CMAKE_GENERATOR_INSTANCE=$VsInstance"
    if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed with exit code $LASTEXITCODE" }

    Step 'Building Release ASI'
    & $CMake --build $BuildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { Fail "CMake build failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
}

if (-not (Test-Path $BuiltAsi)) { Fail "Build reported success but ASI is missing: $BuiltAsi" }
$builtHash = (Get-FileHash -Algorithm SHA256 $BuiltAsi).Hash
$builtSize = (Get-Item $BuiltAsi).Length
Write-Host "`nBuilt ASI : $BuiltAsi"
Write-Host "Size      : $builtSize bytes"
Write-Host "SHA-256   : $builtHash"

if ($BuildOnly) {
    Step 'BuildOnly requested; deployment skipped'
    exit 0
}

if (Get-Process -Name 'CrimsonDesert' -ErrorAction SilentlyContinue) {
    Fail 'CrimsonDesert.exe is running. Close the game before replacing the ASI.'
}
if (-not (Test-Path $GameBin)) { Fail "Game bin64 directory not found: $GameBin" }

if ((Test-Path $DeployedAsi) -and -not $SkipBackup) {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
    $backupDir = Join-Path $BackupRoot "transmog_PHASE3A_$stamp"
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $backupAsi = Join-Path $backupDir 'CrimsonDesertLiveTransmog_PRE_PHASE3A.asi'
    Step "Backing up currently deployed ASI to $backupDir"
    Copy-Item -LiteralPath $DeployedAsi -Destination $backupAsi -Force
    $backupHash = (Get-FileHash -Algorithm SHA256 $backupAsi).Hash
    Set-Content -LiteralPath (Join-Path $backupDir 'SHA256.txt') -Encoding ASCII -Value @(
        "CrimsonDesertLiveTransmog_PRE_PHASE3A.asi  $backupHash",
        "Phase3A candidate                       $builtHash"
    )
}

Step 'Deploying Phase 3A candidate'
Copy-Item -LiteralPath $BuiltAsi -Destination $DeployedAsi -Force
$deployedHash = (Get-FileHash -Algorithm SHA256 $DeployedAsi).Hash
if ($deployedHash -ne $builtHash) {
    Fail "Deployment verification failed. Built hash $builtHash != deployed hash $deployedHash"
}

Write-Host "Deployed  : $DeployedAsi"
Write-Host "SHA-256   : $deployedHash"
Write-Host "Log       : $LogPath"
Write-Host "Discovery : $DiscoveryPath"
Write-Host "`nNext: launch Crimson Desert, open Wardrobe with Home, run the Phase 3A smoke test, then send the log + discovery JSON." -ForegroundColor Green
