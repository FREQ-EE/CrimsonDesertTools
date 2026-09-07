# Phase 3A — First Runtime Findings (2026-09-07)

Branch: `feature/wardrobe-foundation`
Validated build commit: `4bd5da4e17cb1add1e50963cd61fe35103f5fd17`

## Confirmed working

- Windows/VS2022 CI configure, Release build, and ASI verification pass.
- Local build/deploy helper compiles and installs successfully.
- Standalone Wardrobe opens in game.
- Mouse hover preview works; ~150 ms dwell feels appropriate and moving away restores the pinned draft.
- Outfit/preset creation works in the first runtime pass.
- Xbox LB/RB cycles appearance slots and updates the right panel.
- Xbox Y switches Appearance / Colour.
- Xbox B closes/cancels the wardrobe.

## Confirmed problems / changes requested

### Discovery is not historical yet

The current `DiscoveryRegistry` is not an inventory/stash scanner and is not an "ever encountered" database. It currently learns from:

1. the persistent `CrimsonDesertLiveTransmog_discovered.json` file;
2. the hard-coded canonical seed derived from FREQEE's `ludomancy/crimson-desert/CURRENT_STATE.md`;
3. active/staged Live Transmog mappings;
4. catalogue appearances explicitly picked by the user.

Therefore the first runtime registry can strongly resemble current inventory/storage without actually reading either container. The observed file contains only 30 stable internal item names. Boots contain exactly the two rows seen in the UI (`hernand_party_leather_boots`, `orcumer_plate_boots`).

Required direction:
- backfill legitimately known historical gear from FREQEE's project records where safe;
- Phase 3B must add real acquisition/current-inventory observation so future discoveries are learned automatically;
- investigate whether Crimson Desert itself retains a safe, queryable historical acquisition/knowledge list. Do not assume one exists until verified.

### Xbox catalogue navigation

Observed:
- D-pad changes no visible focus and does not visibly move catalogue selection;
- A does not pin the selected appearance;
- X appears to do nothing when no obvious draft is selected;
- LB/RB, Y, B work.

This matches the candidate code: catalogue A was diagnostic-only and `s_catalogNav` had no rendered focus treatment. Next candidate must:
- render a clear controller focus state on the selected appearance tile;
- build controller activation from the same filtered candidate list used for rendering;
- A pins the focused tile;
- D-pad movement respects the actual responsive column count;
- keep X = Apply and B = Cancel/Close.

### Controls legend

Display the full controller map persistently when a controller is connected rather than requiring the user to infer it:

`LB/RB Slot   D-pad Select   A Pin   X Apply   Y Appearance/Colour   View Outfits   B Cancel/Close`

When Outfit focus is active, the legend should say so and show the altered D-pad/A behaviour.

### Ultrawide layout / labels

At the user's ultrawide runtime layout the right panel is needlessly constrained by the current 720 px maximum while the centre has abundant unused width.

Requested changes:
- increase right-panel width on ultrawide displays;
- stop hard-truncating item names at 34 characters;
- prefer a cleaner two-line tile label or wider tile rather than severe clipping;
- replace `DRAFT` / `APPLIED` words with compact visual markers plus tooltip/legend if practical;
- use game-facing terminology consistently (e.g. Helm/Headgear, Chest Armour, Cloak, Gloves, Boots) after checking the actual in-game slot labels.

### Hidden chest / topless expectation

Selecting Hidden for Chest does not make Kliff fully topless. Treat this separately from discovery/controller work: the current transmog hide path removes the managed chest appearance, but the game's base-body/underlayer presentation may still remain. Investigate only after Phase 3A core functionality is stable.

## Next deterministic runtime test

Do not ask the user to improvise around already-known failures. After the next candidate is deployed, run this compact matrix:

1. **Controller focus** — Head or Boots: LB/RB changes slot; D-pad visibly moves focus; A pins; X applies; B cancels/closes; Y toggles tabs; View toggles Outfit focus.
2. **Mouse transaction** — hover A -> move away -> click B -> hover C -> move away -> Cancel; repeat with Apply.
3. **Discovery invariance** — choose one known discovered item, move it between carried inventory and storage, reopen Wardrobe; it must remain discovered because registry persistence is independent of current container location.
4. **New acquisition** — acquire one cheap/new item not already in `discovered.json` without using unrestricted catalogue; after the future acquisition hook lands, it must appear automatically and persist after moving/selling/storing it.
5. **Outfits** — Save new, rename, duplicate, switch, Cancel, Apply, delete disposable duplicate.
6. **Colour** — one normal armour dye; Cancel then Apply. Test Extended only if ColorOverride was enabled before launch.
7. **Close semantics** — make a draft, close via Home/Escape, observe whether draft remains visible while closed, reopen and confirm baseline restoration.
8. **Diagnostics** — press Diagnostics once and return `.log`, `discovered.json`, `presets.json`, plus one screenshot.

## Runtime artefacts received in first pass

- Wardrobe screenshot.
- `CrimsonDesertLiveTransmog_discovered.json`.
- `CrimsonDesertLiveTransmog_presets.json`.

Still useful from this exact run: `CrimsonDesertLiveTransmog.log`, especially `[wardrobe-discovery]`, `[wardrobe-input]`, and `[wardrobe-diag]` lines.
