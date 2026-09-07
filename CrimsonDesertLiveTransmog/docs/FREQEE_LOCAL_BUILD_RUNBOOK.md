# FREQEE Wardrobe — Local Build / Deploy Runbook

This file is the authority for FREQEE's local Windows build. Do not improvise the toolchain when iterating the Wardrobe.

## Known-good machine / toolchain

- Repository: `C:\Users\theod\Documents\GitHub\CrimsonDesertTools`
- Project: `CrimsonDesertLiveTransmog`
- Game runtime: `D:\Steam Library\steamapps\common\Crimson Desert\bin64`
- Visual Studio 2022 Community: `C:\Program Files\Microsoft Visual Studio\2022\Community`
- VS instance version: `17.11.35327.3`
- MSVC: 14.41.34120 / compiler 19.41.34123
- VS2022 bundled CMake:
  `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Bundled CMake version: 3.29.5-msvc4
- Required generator instance string:
  `C:\Program Files\Microsoft Visual Studio\2022\Community,version=17.11.35327.3`
- Configure preset: `msvc-release`
- Build directory: `CrimsonDesertLiveTransmog\build\release-msvc`

VS2022 exists physically but is not registered by the current Visual Studio Installer / `vswhere`. Therefore a bare `Visual Studio 17 2022` generator lookup fails. The explicit instance string above **must include the `,version=17.11.35327.3` suffix**.

## Do not use

- Global CMake 4.4.3 for this target.
- Visual Studio 2026 / MSVC 19.50 for ordinary Wardrobe iteration.
- A guessed `CMAKE_GENERATOR_INSTANCE` containing only the VS directory path.

Observed failures when those paths were tried:

- global CMake 4.4.3 + VS2026: duplicate ImGui/ReShade thunk `LNK2005` / `LNK1169` failures;
- VS2022 generator with no explicit instance: `could not find any instance of Visual Studio`;
- VS2022 generator with path but no version suffix: instance directory exists but is not known to Visual Studio Installer.

None of those failures required a source-code workaround. The exact Phase Two recipe below builds and runs successfully.

## Exact manual recipe

From `CrimsonDesertLiveTransmog`:

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vsInstance = "C:\Program Files\Microsoft Visual Studio\2022\Community,version=17.11.35327.3"

& $cmake --preset msvc-release -D "CMAKE_GENERATOR_INSTANCE=$vsInstance"
& $cmake --build "build\release-msvc" --config Release --parallel
```

Expected binary:

`CrimsonDesertLiveTransmog\build\release-msvc\CrimsonDesertLiveTransmog.asi`

## Normal iteration command

Do not paste a long sequence of build commands into interactive PowerShell. Pull the feature branch and let the checked-in script perform the whole transaction:

```powershell
git pull --ff-only origin feature/wardrobe-foundation; if ($LASTEXITCODE -ne 0) { throw "git pull failed" }; & ".\CrimsonDesertLiveTransmog\scripts\freqee_build_deploy.ps1"
```

`scripts/freqee_build_deploy.ps1` must continue to use the exact recipe above. It also checks the game is closed, updates submodules, backs up runtime files, deploys the ASI and verifies the deployed SHA-256. `_runtime_backups/` is intentionally gitignored.

A successful run must end with:

`WARDROBE V2 BUILD + DEPLOY SUCCEEDED`

Only then launch Crimson Desert.

## CI versus local

GitHub Actions uses a registered `windows-2022` runner and can use the normal VS2022 generator. CI proves source compilation/linking on VS2022, but it does **not** remove the local machine's requirement for the explicit generator-instance string.

## Rule for future Wardrobe work

UI/source iteration should not modify CMake or the local build/deploy recipe unless the project genuinely gains a new build dependency. Keep `src/wardrobe_overlay_v2.cpp` as the stable CMake translation-unit filename; newer Wardrobe implementations can be included from that stable TU so UI revisions do not destabilise the validated build layer.
