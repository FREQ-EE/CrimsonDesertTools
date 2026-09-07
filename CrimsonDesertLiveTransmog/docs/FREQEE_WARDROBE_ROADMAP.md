# FREQEE Wardrobe Development Roadmap

**Status:** planning / Phase Three not started
**Branch:** `freqee/wardrobe`
**Baseline:** upstream Live Transmog v0.15.0 at `6c9ccd18446aae08f60b8b6cc2a29e1fbd886b7f`

This document defines the custom development path for FREQEE's spoiler-safe Crimson Desert wardrobe fork. It is intentionally separate from upstream release documentation.

## Non-negotiable invariants

- `main` remains a clean upstream-tracking branch.
- Custom work branches from `freqee/wardrobe` and merges back into it.
- Normal play must never expose undiscovered item names, models, raw IDs or spoiler placeholders.
- Existing upstream safety/body/slot filtering remains authoritative unless a specific bug is proven.
- Stable transmog application logic should not be rewritten merely for UI style.
- Every release candidate must retain a trivial rollback path to the previous known-good `.asi`.
- Camera/pause reverse engineering must remain isolated from the core wardrobe/discovery logic.

## Phase Three structure

### 3A — Wardrobe foundation

Low-risk/high-confidence work. Recommended as one cohesive branch, `feature/wardrobe-foundation`, with small reviewable commits.

#### A1. Build/deploy helper

Codify the known-good local build environment:
- VS 2022 Community installation: `C:\Program Files\Microsoft Visual Studio\2022\Community`
- explicit CMake generator instance: `C:\Program Files\Microsoft Visual Studio\2022\Community,version=17.11.35327.3`
- preferred CMake: VS-bundled `3.29.5-msvc4`
- do not use global CMake 4.4.3 for this target unless the ImGui duplicate-link issue is re-tested/fixed.

Current known-good binary from untouched baseline:
- size: 3,682,304 bytes
- SHA-256: `887F63291637540FAA1BA2984AC176C2101BED3C8D3BC1056AF6EBFFCC97DB21`

Add tooling only if it reduces error-prone manual steps; do not hard-code a user's game path into product logic.

#### A2. Persisted discovered-appearance registry

Add a dedicated registry persisted outside the full catalogue, for example a small JSON file next to the mod configuration.

Required behaviour:
- normal picker flow: full catalogue → existing slot/safety/body/variant filters → discovered registry → visible rows;
- undiscovered rows are not rendered at all;
- no greyed-out unknowns, `????`, raw IDs, or model previews;
- discovered status survives restart and remains after items are sold/stored;
- malformed/missing registry files fail safely and do not expose the full catalogue;
- a deliberately buried developer/debug override may show the full catalogue, but must be OFF by default and clearly labelled.

The initial registry seed must come only from the latest canonical live save record in `FREQ-EE/ludomancy/crimson-desert/`. Never seed appearances merely because an external database says they exist.

#### A3. Wardrobe UI architecture / re-theme

Replace the developer-tool presentation with a coherent wardrobe structure while retaining the same underlying transmog mechanisms.

Recommended high-level layout:
- equipment-slot selector;
- character viewport / inspection area;
- tabs: **Appearance / Colour / Outfits**;
- explicit Apply / Cancel behaviour;
- current appearance vs preview state;
- unsaved-change indicator;
- clear controller focus/navigation state.

Do not make the UI dependent on ReShade. Standalone overlay remains first-class.

#### A4. Integrated colour controls

Promote the existing colour/dye functionality into the normal wardrobe workflow rather than creating a second independent colour system.

Recommended user-facing modes:
- **Normal:** game-compatible/vanilla-like dye restrictions where practical;
- **Extended:** opt-in arbitrary per-material overrides supported by the existing mod.

Keep the underlying colour modules modular so the experimental renderer/material work remains isolated from ordinary transmog state.

#### A5. Outfit/preset UX

Reuse existing preset infrastructure but present it as wardrobe outfits:
- save current appearance as outfit;
- clone/rename/apply;
- clear indication of active vs modified state;
- no regression to existing per-character persistence.

#### A6. Xbox/controller navigation foundation

Use the existing input/overlay abstraction where possible.

Target:
- open/close wardrobe;
- change equipment slot;
- navigate appearance list;
- change colour controls;
- manage outfits/presets;
- Apply / Cancel;
- later share the same navigation scheme with camera inspection controls.

Preferred opening method is a controller chord if stable. Mapping keyboard Home to an Xbox Elite rear control remains an acceptable fallback if native opening adds disproportionate risk.

### 3B — Automatic discovery

Moderate runtime-integration risk. Do only after 3A is stable.

Objectives:
- observe inventory/equipment acquisition safely;
- auto-learn an appearance when legitimately acquired/equipped;
- persist discovery permanently after selling/storing;
- log discoveries diagnostically before enabling silent automatic learning;
- avoid false positives from catalogue enumeration or hidden game data.

Optional later rule: merchant preview may count as discovery, but this is separate from owned/acquired discovery and should not be enabled implicitly.

### 3C — Inspection mode

Higher reverse-engineering risk and deliberately isolated from core wardrobe logic.

Objectives:
- camera orbit/rotation;
- zoom;
- slot-specific framing;
- full-body framing for outfits;
- controller camera controls;
- preferably manipulate the camera rather than rotating the player actor.

Suggested framing targets:
- helm: head / upper torso;
- chest: torso;
- gloves: upper body / hands, slight oblique angle;
- boots: lower body;
- outfit: full body.

Pause strategy priority:
1. native game/menu pause state;
2. known safe time/gameplay suspension mechanism;
3. selective world/input suppression while rendering continues;
4. invasive techniques only if required.

Do not suspend arbitrary game threads as the default approach.

### 3D — Polish / optional extensions

Only after 3A–3C are stable:
- thumbnails if technically practical;
- favourites;
- richer sorting/search;
- localisation/display-name improvements;
- merchant-preview discovery if wanted;
- optional ReShade-hosted presentation only if it offers a concrete advantage;
- upstream-sync tooling/conflict checks.

## Branch / commit recommendation

When implementation begins:

1. create `feature/wardrobe-foundation` from `freqee/wardrobe`;
2. keep A1–A6 as separate logical commits even if developed in one round;
3. first behaviour-changing commit: discovered registry + picker filter;
4. build after every logical commit;
5. perform runtime smoke tests before merging back to `freqee/wardrobe`;
6. keep camera/pause work on a later separate branch.

## Phase Two build baseline

Phase Two proved a complete untouched source → build → install → runtime path before custom development.

Known-good commands:

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vsInstance = "C:\Program Files\Microsoft Visual Studio\2022\Community,version=17.11.35327.3"

cmake --version
& $cmake --preset msvc-release -D "CMAKE_GENERATOR_INSTANCE=$vsInstance"
& $cmake --build "build\release-msvc" --config Release --parallel
```

Global CMake 4.4.3 configured successfully but failed during final Release link with duplicate ImGui symbols. VS-bundled CMake 3.29.5-msvc4 built the exact same untouched source successfully. Treat this as a project-tooling compatibility constraint until deliberately revisited.
