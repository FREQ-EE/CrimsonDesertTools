# FREQEE Wardrobe Development Roadmap

**Status:** Phase 3A implementation candidate complete; local compile/runtime validation pending  
**Integration branch:** `freqee/wardrobe`  
**Current feature branch:** `feature/wardrobe-foundation`  
**Baseline:** upstream Live Transmog v0.15.0 at `6c9ccd18446aae08f60b8b6cc2a29e1fbd886b7f`

This document defines the custom development path for FREQEE's spoiler-safe Crimson Desert wardrobe fork. It is intentionally separate from upstream release documentation.

## Non-negotiable invariants

- `main` remains a clean upstream-tracking branch.
- Custom work branches from `freqee/wardrobe` and merges back into it only after build/runtime validation.
- Normal play must never expose undiscovered item names, models, raw IDs or spoiler placeholders.
- Existing upstream safety/body/slot filtering remains authoritative unless a specific bug is proven.
- Stable transmog application logic should not be rewritten merely for UI style.
- Every release candidate retains a trivial rollback path to the previous known-good `.asi`.
- Camera/pause reverse engineering remains isolated from core wardrobe/discovery logic.

# Phase Three

## 3A — Wardrobe foundation

### Current implementation

Phase 3A has now been implemented as a candidate on `feature/wardrobe-foundation`. No claim of completion should be made until FREQEE compiles and runs the candidate in-game.

Implemented candidate features:

1. **Validated build/deploy helper**
   - `scripts/freqee_build_deploy.ps1` codifies the known-good VS2022-bundled CMake 3.29.5 path;
   - refuses deployment while Crimson Desert is running;
   - Release build + SHA-256 verification;
   - backs up the currently deployed ASI before replacement;
   - deploys only `CrimsonDesertLiveTransmog.asi`.

2. **Persistent discovered-appearance registry**
   - `src/wardrobe_discovery.hpp` persists stable internal item names in `CrimsonDesertLiveTransmog_discovered.json`;
   - normal catalogue filters through this registry after the upstream slot/body/safety rules;
   - malformed/missing registry fails closed rather than exposing the full catalogue;
   - canonical 2026-09-07 acquired gear is seeded by display-name resolution, with duplicate/variant/body guards;
   - existing staged/applied transmog item mappings are also learned;
   - user picks are persisted immediately by stable internal name.

3. **Inventory-inspired Wardrobe UI**
   - old developer panel replaced on the feature branch by a left / transparent-centre / right wardrobe layout;
   - left: character editing selector, body-filter affordance, equipment-slot selector, Outfits;
   - centre: intentionally unobstructed live character viewport, reserved for Phase 3C camera work;
   - right: Appearance / Colour tabs, discovered catalogue, search, filters and contextual actions;
   - standalone root uses no opaque fullscreen background.

4. **Transactional wardrobe semantics**
   - no user-facing `Instant Apply` mode;
   - Applied state = wardrobe-open persisted baseline;
   - Draft = clicked/pinned state;
   - Hover preview = temporary single-slot apply after ~150 ms dwell;
   - leaving hover returns to the pinned draft;
   - Apply is the persistence boundary via `PresetManager::replace_current_from_state()`;
   - Cancel restores wardrobe-open preset + mappings;
   - `Equipped` (no override) and `Hidden` (active + itemId 0) are separate choices.

5. **Spoiler control**
   - default control is `Catalogue: Discovered`;
   - unrestricted catalogue requires an explicit spoiler confirmation;
   - unrestricted mode is session-only;
   - raw/internal IDs remain hidden in normal mode.

6. **Outfit UX**
   - existing PresetManager remains the sole persistence system;
   - list/select, save current as new, rename, duplicate, delete;
   - selecting another Outfit changes the draft context;
   - Cancel restores the Outfit active when Wardrobe opened;
   - Apply persists the currently selected/current draft.

7. **Integrated colour entry point**
   - `Colour` is a first-class right-panel tab;
   - normal dye remains upstream `DyeRecordInject` / ARMOR_MOD state;
   - Extended material colour remains upstream ColorOverride;
   - no parallel colour engine was added;
   - startup-gated `[Experimental] ColorOverride=true` behaviour is preserved.

8. **Xbox/controller foundation**
   - dynamic XInput polling avoids a new static link dependency;
   - Y: Appearance/Colour;
   - View/Back: catalogue/outfit navigation context;
   - LB/RB: slots or outfits;
   - D-pad navigation state;
   - X: Apply;
   - B: Cancel + close;
   - A selects Outfits;
   - catalogue A-selection is intentionally left as a diagnostic probe until the first runtime navigation test establishes the correct filtered-list activation path.

9. **Diagnostics**
   - `[wardrobe-discovery]`, `[wardrobe]`, `[wardrobe-ui]`, `[wardrobe-input]` and `[wardrobe-diag]` log families added;
   - Diagnostics button emits a compact state/layout line for runtime handoff.

### Runtime validation document

See `docs/PHASE3A_RUNTIME_TEST.md` for the exact compile/deploy/smoke-test procedure and artefacts to return.

### Still inside the 3A validation loop

These are not promoted to 3B/3C; they simply require a live build or runtime observation before finalising 3A:

- compiler/linker verification of the new ImGui surface against the ReShade function-table wrapper;
- catalogue A-button activation for controller navigation;
- immediate Cancel callback on the legacy Home/Escape standalone-overlay close path;
- 5120×1440 GDI dirty-rectangle performance with left + right panels;
- final visual fit of the existing normal/Extended dye popup inside the new right panel.

If the first build fails, fix compile issues on this feature branch before changing behaviour. If it builds but a runtime item above fails, use the diagnostic log rather than speculative reverse engineering.

## 3B — Automatic discovery

Do only after 3A is stable.

Objectives:
- observe inventory/equipment acquisition safely;
- auto-learn an appearance when legitimately acquired/equipped;
- persist discovery permanently after selling/storing;
- log discoveries diagnostically before enabling silent automatic learning;
- avoid false positives from catalogue enumeration or hidden game data.

Optional later rule: merchant preview may count as discovery, but this is separate from owned/acquired discovery and should not be enabled implicitly.

## 3C — Inspection mode

Higher reverse-engineering risk and deliberately isolated from core wardrobe logic.

Objectives:
- camera orbit/rotation;
- zoom;
- slot-specific framing;
- smooth framing transitions;
- full-body framing for Outfits;
- controller camera controls;
- preferably manipulate the camera rather than rotating the player actor;
- inactive-character 3D preview only if the engine exposes a safe path.

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

## 3D — Polish / optional extensions

Only after 3A–3C are stable:
- actual game item thumbnails if a safe texture-resource path is identified;
- favourites;
- richer sorting/search/recent-discovery indicators;
- localisation/display-name improvements;
- merchant-preview discovery if wanted;
- optional ReShade-hosted presentation only if it provides a concrete advantage;
- upstream-sync tooling/conflict checks.

## Branch / merge discipline

Current path:

1. `feature/wardrobe-foundation` from `freqee/wardrobe` — DONE;
2. Phase 3A implementation commits — candidate exists;
3. local compile with known-good toolchain — NEXT;
4. runtime smoke test + return log/discovery JSON/screenshot — NEXT;
5. fix candidate in small commits;
6. only after validation, merge `feature/wardrobe-foundation` into `freqee/wardrobe`;
7. automatic discovery and camera/pause remain later branches.

## Phase Two build baseline

Known-good commands remain:

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vsInstance = "C:\Program Files\Microsoft Visual Studio\2022\Community,version=17.11.35327.3"

& $cmake --preset msvc-release -D "CMAKE_GENERATOR_INSTANCE=$vsInstance"
& $cmake --build "build\release-msvc" --config Release --parallel
```

Global CMake 4.4.3 configured successfully but failed during final Release link with duplicate ImGui symbols. VS-bundled CMake 3.29.5-msvc4 built the exact untouched baseline successfully. Treat that as a project-tooling compatibility constraint until deliberately revisited.
