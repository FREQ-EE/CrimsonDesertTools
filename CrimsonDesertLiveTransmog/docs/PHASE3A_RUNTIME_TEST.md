# Phase 3A — Wardrobe Foundation Runtime Test

**Branch:** `feature/wardrobe-foundation`  
**Status:** implementation candidate; compile + runtime validation required before merge into `freqee/wardrobe`.

This test is deliberately diagnostic. Phase 3A changes the standalone ImGui presentation and transaction semantics but keeps the proven transmog/carrier/dye apply machinery underneath.

## 1. Build and deploy

Close Crimson Desert first.

```powershell
cd 'C:\Users\theod\Documents\GitHub\CrimsonDesertTools'
git fetch origin
git switch feature/wardrobe-foundation
git pull --ff-only

& '.\CrimsonDesertLiveTransmog\scripts\freqee_build_deploy.ps1'
```

The helper:
- requires the validated VS2022-bundled CMake 3.29.5 path;
- refuses to deploy while `CrimsonDesert.exe` is running;
- builds Release;
- hashes the candidate;
- backs up the currently deployed ASI under `Documents\Crimson Desert Backups\transmog_PHASE3A_<timestamp>`;
- replaces only `bin64\CrimsonDesertLiveTransmog.asi`;
- verifies the deployed SHA-256.

Use `-BuildOnly` to compile without touching the game install.

## 2. Launch / initial safety

1. Launch Crimson Desert normally.
2. Wait until the world is fully loaded.
3. Press Home once.
4. Do **not** enable `Catalogue: ALL - SPOILERS` during the spoiler-safety test.

Expected:
- Wardrobe opens instead of the old developer panel.
- The centre of the game remains visually unobstructed.
- Left panel: character + slot selector + Outfits.
- Right panel: selected slot + Appearance / Colour + catalogue + Apply / Cancel / Reset slot.
- A file named `CrimsonDesertLiveTransmog_discovered.json` appears in `bin64`.
- The log contains `[wardrobe-discovery] ready` and `[wardrobe-ui] layout`.

## 3. Spoiler-safe catalogue

Check several slots: Head, Chest, Hands, Boots, weapons/shields if enabled.

Expected:
- only appearances already present in the discovery registry are shown;
- no `????`, raw undiscovered IDs, NPC/body variants or unknown names appear in normal mode;
- `Equipped` and `Hidden` are always distinct first-class choices;
- search searches only the already-visible discovered set.

Then click `Catalogue: Discovered` once.

Expected:
- a warning explicitly says that unrestricted catalogue mode reveals undiscovered gear;
- cancelling leaves normal mode unchanged;
- only deliberate confirmation enables `Catalogue: ALL - SPOILERS`;
- unrestricted mode is session-only and should not silently become the normal default.

## 4. Draft / preview transaction

For one slot with at least two discovered appearances:

1. Note the currently applied appearance.
2. Hover another tile for at least ~150 ms.
3. Move the pointer away without clicking.
4. Click the alternative tile.
5. Hover a third tile if available; move away.
6. Click **Cancel**.

Expected:
- hover temporarily changes only that slot;
- moving away restores the pinned draft;
- clicking pins a draft and marks the wardrobe as having unapplied changes;
- after pinning, hovering another item does not destroy the clicked draft;
- Cancel restores the appearance/state from wardrobe-open.

Repeat, but click **Apply** instead of Cancel.

Expected:
- Apply commits the draft to the active Outfit/preset;
- the dirty indicator clears;
- close/reopen Wardrobe and restart the game: the applied appearance persists.

## 5. Equipped versus Hidden

On an armour slot:
- choose **Equipped**: the real equipped item's normal appearance should show (no transmog override);
- choose **Hidden**: the slot should be deliberately hidden through the upstream active+id0 path.

Report any slot where the semantics differ. Lantern is deliberately special upstream: hiding/clearing it can remove the light source, so do not use Lantern for this test.

## 6. Outfits

Test:
- select another saved Outfit;
- Cancel back to the wardrobe-open Outfit;
- select another Outfit and Apply;
- Save current as new;
- Rename;
- Duplicate;
- Delete the disposable duplicate.

Expected:
- selecting an Outfit previews/loads it as the current draft context;
- Cancel restores the wardrobe-open Outfit;
- Apply makes the selected/current Outfit the persisted state;
- rename/duplicate/delete continue to use PresetManager persistence rather than a second storage system.

## 7. Colour

Test one armour slot.

### Normal
Open Colour and enter the existing Dye UI. Change a normal dye channel, then Cancel the wardrobe session. Repeat and Apply.

Expected:
- normal ARMOR_MOD dye functionality still works;
- Cancel discards unsaved colour edits via the existing preset snapshot path;
- Apply persists them.

### Extended
The upstream ColorOverride hook is startup-gated. If `[Experimental] ColorOverride=true` is already enabled, test a per-material override. If it is false, record that fact; do not change it mid-session and expect the hook to appear.

Expected when enabled:
- existing per-material swatch controls remain accessible through the integrated Colour flow;
- Apply saves swatches through the existing preset manager.

## 8. Controller probe

Connect the Xbox controller before opening Wardrobe.

Implemented now:
- Y: Appearance / Colour tab;
- View/Back: switch catalogue/outfit navigation context;
- LB/RB: slot cycle (catalogue context) or outfit cycle (outfit context);
- D-pad: navigation cursor state;
- X: Apply;
- B: Cancel and close;
- A: Outfit selection works; **catalogue A-selection is deliberately diagnostic in this first candidate** and logs the requested nav index rather than guessing an item mapping before runtime verification.

Expected:
- log contains `[wardrobe-input] XInput controller detected`;
- no game crash or stuck input;
- send the log after exercising the controls so catalogue activation can be completed against observed behaviour.

## 9. Close semantics probe

B on controller explicitly cancels before closing. The standalone overlay's legacy Home/Escape toggle is still owned by `dx_overlay.cpp` and does not yet expose a pre-close callback to Wardrobe.

Probe:
1. make a draft change;
2. close with Escape;
3. observe the character while the wardrobe is closed;
4. reopen Wardrobe.

The Phase 3A candidate currently detects the render gap on reopen and restores the abandoned baseline then. Record whether the draft remains visible during the closed interval. If so, the next patch will add an explicit close callback to the standalone overlay toggle rather than guessing before runtime validation.

## 10. Ultrawide / performance

At 5120×1440:
- move pointer rapidly between left panel, centre and right panel;
- scroll a populated catalogue;
- hover-preview several items;
- leave the UI idle for ~10 s.

Record:
- any visible input lag;
- frame-rate impact relative to stock v0.15.0 overlay;
- whether the GDI dirty-rectangle union becomes expensive because the left and right panels span the screen.

## 11. Diagnostics to return

After the test, send:

1. `D:\Steam Library\steamapps\common\Crimson Desert\bin64\CrimsonDesertLiveTransmog.log`
2. `D:\Steam Library\steamapps\common\Crimson Desert\bin64\CrimsonDesertLiveTransmog_discovered.json`
3. one screenshot with Wardrobe open at 5120×1440 (or the actual current game resolution)
4. a short note for anything visibly wrong.

Also press **Diagnostics** in Wardrobe once before collecting the log. It emits a compact `[wardrobe-diag]` line with layout, character, slot, discovery count, catalogue mode, ColorOverride state, controller state and dirty state.

## Runtime-dependent items intentionally not claimed complete yet

These remain inside the Phase 3A validation loop rather than being promoted to later phases:
- compile compatibility of the new ImGui surface against the ReShade function-table wrapper;
- catalogue A-button activation mapping for XInput navigation;
- immediate Cancel-on-Home/Escape close callback;
- exact 5120×1440 dirty-rectangle/GDI cost;
- visual fit of the normal/Extended colour popup inside the redesigned right panel.

Camera orbit, slot-specific framing, inactive-character 3D preview and true game pausing remain Phase 3C and are not part of this test.
