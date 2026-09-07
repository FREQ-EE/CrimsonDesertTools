// wardrobe_overlay_v3.cpp -- FREQEE Wardrobe UI/runtime refinement pass.
//
// Goals of this pass:
// - preserve the proven transmog/preset/dye engine and transactional Applied/Draft/Preview model;
// - make the standalone surface read like a deliberate in-game wardrobe rather than default ImGui tooling;
// - keep the no-icon presentation as a proper list (the runtime icon probe found no easy item-icon path);
// - make controller focus behave like hover preview;
// - make Outfits and the spoiler catalogue control fully reachable from Xbox input;
// - expose the existing normal ARMOR_MOD dye picker clearly even when experimental ColorOverride is disabled;
// - learn currently EQUIPPED real items through the already-understood auth table without learning UI previews.

#include "overlay.hpp"
#include "dx_overlay.hpp"
#include "overlay_ui/dye_popup.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"
#include "color_override/color_override.hpp"
#include "color_override/color_reinit.hpp"
#include "constants.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"
#include "transmog_map.hpp"
#include "wardrobe_discovery.hpp"
#include "wardrobe_equipped_learning.hpp"
#include "wardrobe_icon_probe.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace Transmog
{
    namespace
    {
        using Wardrobe::DiscoveryRegistry;

        constexpr ImVec4 kIvory{0.95f, 0.93f, 0.87f, 1.00f};
        constexpr ImVec4 kGold{0.91f, 0.70f, 0.39f, 1.00f};
        constexpr ImVec4 kGoldDim{0.33f, 0.245f, 0.12f, 0.94f};
        constexpr ImVec4 kPanel{0.028f, 0.032f, 0.031f, 0.93f};
        constexpr ImVec4 kPanelSoft{0.042f, 0.047f, 0.045f, 0.91f};
        constexpr ImVec4 kBorder{0.78f, 0.73f, 0.63f, 0.42f};
        constexpr ImVec4 kMuted{0.68f, 0.66f, 0.60f, 1.00f};
        constexpr ImVec4 kDanger{0.90f, 0.52f, 0.45f, 1.00f};
        constexpr ImVec4 kFocus{0.50f, 0.37f, 0.16f, 0.96f};
        constexpr ImVec4 kFocusHover{0.61f, 0.45f, 0.20f, 0.98f};

        enum class WardrobeTab
        {
            Appearance,
            Colour,
        };

        enum class OutfitAction : int
        {
            Load = 0,
            SaveNew,
            Rename,
            Duplicate,
            Delete,
            Count,
        };

        struct WardrobeSession
        {
            bool active = false;
            std::int64_t lastDrawMs = 0;
            int baselinePreset = 0;
            std::array<SlotMapping, k_slotCount> baseline{};
            std::array<SlotMapping, k_slotCount> draft{};
            bool previewActive = false;
            std::size_t previewSlot = 0;
            SlotMapping previewMapping{};
            std::uint16_t hoverPendingId = 0;
            bool hoverPendingActive = false;
            std::int64_t hoverStartMs = 0;
        };

        struct PadState
        {
            bool connected = false;
            WORD down = 0;
            WORD pressed = 0;
        };

        struct CatalogueCandidate
        {
            std::string label;
            std::string stableId;
            SlotMapping mapping{};
        };

        WardrobeSession s_session{};
        WardrobeTab s_tab = WardrobeTab::Appearance;
        std::size_t s_selectedSlot = 0;
        bool s_showAllCatalogue = false;
        int s_catalogNav = 0;
        int s_outfitNav = 0;
        int s_outfitActionNav = static_cast<int>(OutfitAction::Load);
        int s_lastVisibleCount = 0;
        bool s_padFocusOutfits = false;
        bool s_padCatalogueFocus = false;
        bool s_loggedLayout = false;
        bool s_loggedPad = false;
        bool s_openDyeRequested = false;

        bool s_spoilerPopupRequest = false;
        bool s_spoilerPopupActive = false;
        bool s_spoilerAccept = false;
        bool s_spoilerCancel = false;

        char s_renameBuf[64]{};
        int s_renameIndex = -1;
        bool s_renameOpen = false;
        bool s_renameCancelRequested = false;

        [[nodiscard]] std::int64_t now_ms() noexcept
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        [[nodiscard]] bool mapping_equal(const SlotMapping &a, const SlotMapping &b) noexcept
        {
            return a.active == b.active && a.targetItemId == b.targetItemId;
        }

        [[nodiscard]] bool mapping_arrays_equal(const std::array<SlotMapping, k_slotCount> &a,
                                                const std::array<SlotMapping, k_slotCount> &b) noexcept
        {
            for (std::size_t i = 0; i < k_slotCount; ++i)
                if (!mapping_equal(a[i], b[i]))
                    return false;
            return true;
        }

        [[nodiscard]] const char *wardrobe_slot_name(TransmogSlot slot) noexcept
        {
            switch (slot)
            {
            case TransmogSlot::Helm: return "Helm";
            case TransmogSlot::Chest: return "Armor";
            case TransmogSlot::Cloak: return "Cloak";
            case TransmogSlot::Gloves: return "Gloves";
            case TransmogSlot::Boots: return "Boots";
            case TransmogSlot::Earring1: return "Earring I";
            case TransmogSlot::Earring2: return "Earring II";
            case TransmogSlot::Necklace: return "Necklace";
            case TransmogSlot::Ring1: return "Ring I";
            case TransmogSlot::Ring2: return "Ring II";
            case TransmogSlot::Lantern: return "Lantern";
            case TransmogSlot::Glasses: return "Glasses";
            case TransmogSlot::Mask: return "Mask";
            case TransmogSlot::Backpack: return "Backpack";
            case TransmogSlot::Bracelet: return "Bracelet";
            case TransmogSlot::MainHand: return "Main Hand";
            case TransmogSlot::OffHand: return "Off Hand";
            case TransmogSlot::Ranged: return "Ranged";
            case TransmogSlot::SubWeapon: return "Sub Weapon";
            case TransmogSlot::TwoHandWeapon: return "Two Hand";
            case TransmogSlot::Tool: return "Tool";
            case TransmogSlot::OffHand2: return "Off Hand II";
            case TransmogSlot::Ranged2: return "Ranged II";
            default: return "Appearance";
            }
        }

        [[nodiscard]] const char *outfit_action_name(OutfitAction action) noexcept
        {
            switch (action)
            {
            case OutfitAction::Load: return "Load";
            case OutfitAction::SaveNew: return "Save New";
            case OutfitAction::Rename: return "Rename";
            case OutfitAction::Duplicate: return "Duplicate";
            case OutfitAction::Delete: return "Delete";
            default: return "Load";
            }
        }

        [[nodiscard]] bool is_armor_slot(TransmogSlot slot) noexcept
        {
            return slot == TransmogSlot::Helm || slot == TransmogSlot::Chest || slot == TransmogSlot::Cloak ||
                   slot == TransmogSlot::Gloves || slot == TransmogSlot::Boots;
        }

        [[nodiscard]] std::vector<std::size_t> enabled_slots()
        {
            std::vector<std::size_t> out;
            out.reserve(k_slotCount);
            for (std::size_t i = 0; i < k_slotCount; ++i)
                if (slot_enabled(i))
                    out.push_back(i);
            return out;
        }

        void ensure_selected_slot()
        {
            if (s_selectedSlot < k_slotCount && slot_enabled(s_selectedSlot))
                return;
            for (std::size_t i = 0; i < k_slotCount; ++i)
                if (slot_enabled(i))
                {
                    s_selectedSlot = i;
                    return;
                }
            s_selectedSlot = 0;
        }

        void force_slot_apply(std::size_t slot)
        {
            if (slot >= k_slotCount)
                return;
            force_apply_pending()[slot] = true;
            flag_enabled().store(true, std::memory_order_relaxed);
            manual_apply_slot(slot);
        }

        void restore_hover_preview()
        {
            if (!s_session.previewActive || s_session.previewSlot >= k_slotCount)
                return;
            slot_mappings()[s_session.previewSlot] = s_session.draft[s_session.previewSlot];
            force_slot_apply(s_session.previewSlot);
            s_session.previewActive = false;
            s_session.hoverStartMs = 0;
        }

        void preview_mapping(std::size_t slot, const SlotMapping &mapping)
        {
            if (slot >= k_slotCount)
                return;
            if (s_session.previewActive && s_session.previewSlot == slot &&
                mapping_equal(s_session.previewMapping, mapping))
                return;
            slot_mappings()[slot] = mapping;
            s_session.previewActive = true;
            s_session.previewSlot = slot;
            s_session.previewMapping = mapping;
            force_slot_apply(slot);
        }

        void pin_draft_mapping(std::size_t slot, const SlotMapping &mapping)
        {
            if (slot >= k_slotCount)
                return;
            s_session.previewActive = false;
            s_session.hoverStartMs = 0;
            s_session.draft[slot] = mapping;
            slot_mappings()[slot] = mapping;
            force_slot_apply(slot);
        }

        void sync_session_to_current(PresetManager &pm)
        {
            s_session.baselinePreset = std::max(0, pm.active_preset_index());
            s_session.baseline = slot_mappings();
            s_session.draft = s_session.baseline;
            s_session.previewActive = false;
            s_session.hoverStartMs = 0;
            s_outfitNav = std::max(0, pm.active_preset_index());
        }

        void begin_session(PresetManager &pm)
        {
            ensure_selected_slot();
            DiscoveryRegistry::instance().ensure_ready();
            Wardrobe::learn_currently_equipped();
            s_session.active = true;
            sync_session_to_current(pm);
            s_catalogNav = 0;
            s_padFocusOutfits = false;
            s_padCatalogueFocus = false;
            DMK::Logger::get_instance().info(
                "[wardrobe] v3 session begin: character='{}' outfit={} discovered={}", pm.editing_character(),
                pm.active_preset_index(), DiscoveryRegistry::instance().size());
        }

        [[nodiscard]] bool session_dirty(PresetManager &pm)
        {
            return !mapping_arrays_equal(s_session.draft, s_session.baseline) ||
                   pm.active_preset_index() != s_session.baselinePreset ||
                   dye_dirty().load(std::memory_order_acquire);
        }

        void cancel_session(PresetManager &pm, bool startFresh = true)
        {
            restore_hover_preview();
            if (pm.preset_count() > 0)
                pm.set_active_preset(std::clamp(s_session.baselinePreset, 0, pm.preset_count() - 1));
            slot_mappings() = s_session.baseline;
            for (std::size_t i = 0; i < k_slotCount; ++i)
                if (slot_enabled(i))
                    force_apply_pending()[i] = true;
            flag_enabled().store(true, std::memory_order_relaxed);
            manual_apply();
            DMK::Logger::get_instance().info("[wardrobe] draft cancelled; restored outfit {}", s_session.baselinePreset);
            s_session.active = false;
            if (startFresh)
                begin_session(pm);
        }

        void apply_session(PresetManager &pm)
        {
            restore_hover_preview();
            slot_mappings() = s_session.draft;
            for (std::size_t i = 0; i < k_slotCount; ++i)
                if (slot_enabled(i))
                    force_apply_pending()[i] = true;
            flag_enabled().store(true, std::memory_order_relaxed);
            manual_apply();
            pm.replace_current_from_state();
            sync_session_to_current(pm);
            DMK::Logger::get_instance().info("[wardrobe] applied and saved: character='{}' outfit={}",
                                             pm.editing_character(), pm.active_preset_index());
        }

        [[nodiscard]] ItemNameTable::BodyKind editing_body_kind(PresetManager &pm)
        {
            using BK = ItemNameTable::BodyKind;
            const std::string ov = pm.body_kind_of(pm.editing_character());
            if (ov == "Male") return BK::Male;
            if (ov == "Female") return BK::Female;
            if (ov == "Both") return BK::Both;
            return ItemNameTable::body_kind_for_character(pm.editing_character());
        }

        [[nodiscard]] bool entry_visible(const ItemNameTable::Entry &e, TransmogSlot slot, PresetManager &pm,
                                         const SlotUIState &filters)
        {
            using BK = ItemNameTable::BodyKind;
            if (!slots_share_picker(e.category, slot))
                return false;
            const bool armor = is_armor_slot(slot);
            const bool nonHumanoid = e.bodyKind == BK::NonHumanoid;
            const bool incompatible = e.category == TransmogSlot::Count || nonHumanoid;
            if (armor && filters.hideIncompatible && incompatible)
                return false;
            if (armor && filters.hideVariants && e.hasVariantMeta)
                return false;
            if (armor && filters.hideBodyMismatch)
            {
                const auto body = editing_body_kind(pm);
                const bool ambiguous = e.bodyKind == BK::Ambiguous;
                const bool match = !nonHumanoid &&
                    (ambiguous || e.bodyKind == BK::Generic || e.bodyKind == BK::Both ||
                     body == BK::Generic || e.bodyKind == body);
                if (!match)
                    return false;
            }
            if (!s_showAllCatalogue && !DiscoveryRegistry::instance().contains(e))
                return false;
            const auto &ui = s_slotUI[s_selectedSlot];
            return name_contains_ci(e.displayName, ui.searchBuf) || name_contains_ci(e.name, ui.searchBuf);
        }

        [[nodiscard]] std::vector<CatalogueCandidate> build_catalogue_candidates(PresetManager &pm)
        {
            std::vector<CatalogueCandidate> out;
            out.reserve(128);
            out.push_back({"Equipped", "equipped", SlotMapping{false, 0}});
            out.push_back({"Hidden", "hidden", SlotMapping{true, 0}});

            const auto slot = static_cast<TransmogSlot>(s_selectedSlot);
            const auto &table = ItemNameTable::instance();
            const auto &filters = s_slotUI[s_selectedSlot];
            if (table.ready())
                for (const auto &e : table.sorted_entries())
                    if (entry_visible(e, slot, pm, filters))
                    {
                        CatalogueCandidate c;
                        c.label = e.displayName.empty() ? e.name : e.displayName;
                        c.stableId = e.name;
                        c.mapping = SlotMapping{true, e.id};
                        out.push_back(std::move(c));
                    }
            return out;
        }

        PadState poll_gamepad()
        {
            using GetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
            static HMODULE lib = nullptr;
            static GetStateFn getState = nullptr;
            static WORD previous = 0;
            static bool attempted = false;
            if (!attempted)
            {
                attempted = true;
                for (const wchar_t *name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
                {
                    lib = LoadLibraryW(name);
                    if (!lib)
                        continue;
                    getState = reinterpret_cast<GetStateFn>(GetProcAddress(lib, "XInputGetState"));
                    if (getState)
                        break;
                    FreeLibrary(lib);
                    lib = nullptr;
                }
            }
            PadState out{};
            if (!getState)
                return out;
            XINPUT_STATE xs{};
            if (getState(0, &xs) != ERROR_SUCCESS)
            {
                previous = 0;
                return out;
            }
            out.connected = true;
            out.down = xs.Gamepad.wButtons;
            out.pressed = static_cast<WORD>(out.down & ~previous);
            previous = out.down;
            return out;
        }

        void cycle_slot(int direction)
        {
            restore_hover_preview();
            const auto slots = enabled_slots();
            if (slots.empty())
                return;
            auto it = std::find(slots.begin(), slots.end(), s_selectedSlot);
            std::size_t pos = it == slots.end() ? 0 : static_cast<std::size_t>(std::distance(slots.begin(), it));
            pos = direction > 0 ? (pos + 1) % slots.size() : (pos + slots.size() - 1) % slots.size();
            s_selectedSlot = slots[pos];
            s_catalogNav = 0;
            s_padCatalogueFocus = true;
            s_openDyeRequested = false;
        }

        void switch_character(PresetManager &pm, int direction)
        {
            const auto names = pm.character_names();
            if (names.empty())
                return;
            cancel_session(pm, false);
            int idx = 0;
            for (int i = 0; i < static_cast<int>(names.size()); ++i)
                if (names[static_cast<std::size_t>(i)] == pm.editing_character())
                    idx = i;
            idx = (idx + direction + static_cast<int>(names.size())) % static_cast<int>(names.size());
            pm.set_editing_character(names[static_cast<std::size_t>(idx)]);
            for (auto &m : slot_mappings())
                m = {};
            pm.apply_to_state();
            manual_apply();
            begin_session(pm);
        }

        void cycle_body_filter(PresetManager &pm)
        {
            static constexpr const char *kKinds[] = {"Auto", "Male", "Female", "Both"};
            const auto cur = pm.body_kind_of(pm.editing_character());
            int idx = 0;
            for (int i = 0; i < 4; ++i)
                if (cur == kKinds[i])
                    idx = i;
            pm.set_body_kind_of(pm.editing_character(), kKinds[(idx + 1) % 4]);
        }

        void preview_outfit(PresetManager &pm, int index)
        {
            restore_hover_preview();
            if (index < 0 || index >= pm.preset_count())
                return;
            pm.set_active_preset(index);
            manual_apply();
            s_session.draft = slot_mappings();
            s_outfitNav = index;
            s_catalogNav = 0;
            DMK::Logger::get_instance().info("[wardrobe] outfit preview index={} name='{}'", index,
                                             pm.active_preset() ? pm.active_preset()->name : std::string{});
        }

        void rename_outfit(PresetManager &pm, int index)
        {
            if (index < 0 || index >= pm.preset_count())
                return;
            const auto &presets = pm.presets();
            std::snprintf(s_renameBuf, sizeof(s_renameBuf), "%s", presets[static_cast<std::size_t>(index)].name.c_str());
            s_renameIndex = index;
            s_renameOpen = true;
            ImGui::OpenPopup("Rename outfit###wardrobe_rename");
        }

        void run_outfit_action(PresetManager &pm, OutfitAction action)
        {
            const int count = pm.preset_count();
            const int target = count > 0 ? std::clamp(s_outfitNav, 0, count - 1) : -1;
            switch (action)
            {
            case OutfitAction::Load:
                if (target >= 0)
                    preview_outfit(pm, target);
                break;
            case OutfitAction::SaveNew:
                restore_hover_preview();
                slot_mappings() = s_session.draft;
                pm.save_as_new_from_state();
                sync_session_to_current(pm);
                break;
            case OutfitAction::Rename:
                if (target >= 0)
                    rename_outfit(pm, target);
                break;
            case OutfitAction::Duplicate:
                if (target >= 0)
                {
                    preview_outfit(pm, target);
                    pm.duplicate_current();
                    manual_apply();
                    sync_session_to_current(pm);
                }
                break;
            case OutfitAction::Delete:
                if (target >= 0)
                {
                    preview_outfit(pm, target);
                    pm.remove_current();
                    if (pm.preset_count() > 0)
                        manual_apply();
                    else
                        manual_clear();
                    sync_session_to_current(pm);
                }
                break;
            default:
                break;
            }
        }

        void draw_rename_popup(PresetManager &pm)
        {
            if (!s_renameOpen)
                return;
            if (ImGui::BeginPopupModal("Rename outfit###wardrobe_rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                if (s_renameCancelRequested)
                {
                    s_renameCancelRequested = false;
                    s_renameOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    ImGui::SetNextItemWidth(420.0f);
                    const bool enter = ImGui::InputText("##outfit_name", s_renameBuf, sizeof(s_renameBuf),
                                                        ImGuiInputTextFlags_EnterReturnsTrue, nullptr, nullptr);
                    if (enter || ImGui::Button("Save name", ImVec2(150.0f, 0.0f)))
                    {
                        if (s_renameIndex >= 0 && s_renameIndex < pm.preset_count())
                        {
                            const int restore = pm.active_preset_index();
                            pm.set_active_preset(s_renameIndex);
                            if (auto *p = pm.active_preset_mut())
                                p->name = s_renameBuf;
                            if (restore >= 0 && restore < pm.preset_count())
                                pm.set_active_preset(restore);
                            pm.save();
                        }
                        s_renameOpen = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel##rename", ImVec2(110.0f, 0.0f)))
                    {
                        s_renameOpen = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
        }

        void log_diagnostics(PresetManager &pm, const PadState &pad)
        {
            const auto &io = ImGui::GetIO();
            DMK::Logger::get_instance().info(
                "[wardrobe-diag] v3 display={}x{} standalone={} char='{}' controlled='{}' preset={}/{} slot={} "
                "discovered={} visible={} showAll={} colorOverride={} gamepad={} dirty={} padContext={} outfitAction={}",
                static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y), s_standaloneMode,
                pm.editing_character(), pm.active_character(), pm.active_preset_index(), pm.preset_count(),
                wardrobe_slot_name(static_cast<TransmogSlot>(s_selectedSlot)), DiscoveryRegistry::instance().size(),
                s_lastVisibleCount, s_showAllCatalogue, flag_color_override().load(std::memory_order_relaxed),
                pad.connected, session_dirty(pm), s_padFocusOutfits ? "outfits" : "catalogue",
                outfit_action_name(static_cast<OutfitAction>(s_outfitActionNav)));
        }

        void section_label(const char *text, bool active = false)
        {
            if (active)
                ui_text_colored(kGold, "%s", text);
            else
                ui_text_colored(kIvory, "%s", text);
        }

        void draw_character_header(PresetManager &pm)
        {
            ui_text_disabled("CHARACTER");
            if (ImGui::Button("<##character_prev", ImVec2(46.0f, 42.0f)))
                switch_character(pm, -1);
            ImGui::SameLine();
            ui_text_colored(kIvory, "%s", pm.editing_character().c_str());
            ImGui::SameLine();
            if (ImGui::Button(">##character_next", ImVec2(46.0f, 42.0f)))
                switch_character(pm, +1);

            const auto body = pm.body_kind_of(pm.editing_character());
            char bodyLabel[64];
            std::snprintf(bodyLabel, sizeof(bodyLabel), "Body: %s##body_cycle", body.c_str());
            if (ImGui::Button(bodyLabel, ImVec2(180.0f, 38.0f)))
                cycle_body_filter(pm);
            if (ImGui::IsItemHovered())
                ui_tooltip("Picker body filter only. Auto follows the protagonist's normal body type.");
        }

        void draw_slot_selector()
        {
            section_label("APPEARANCE SLOT");
            const auto slots = enabled_slots();
            const float avail = ImGui::GetContentRegionAvail().x;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float width = std::max(110.0f, (avail - gap) * 0.5f);
            int col = 0;
            for (const auto idx : slots)
            {
                if (col == 1)
                    ImGui::SameLine();
                const bool selected = idx == s_selectedSlot;
                if (selected)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, kGoldDim);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kFocusHover);
                }
                char label[96];
                std::snprintf(label, sizeof(label), "%s##slot_%zu",
                              wardrobe_slot_name(static_cast<TransmogSlot>(idx)), idx);
                if (ImGui::Button(label, ImVec2(width, 46.0f)))
                {
                    restore_hover_preview();
                    s_selectedSlot = idx;
                    s_catalogNav = 0;
                    s_padCatalogueFocus = false;
                    s_openDyeRequested = false;
                }
                if (selected)
                    ImGui::PopStyleColor(2);
                col = 1 - col;
            }
        }

        void draw_outfits(PresetManager &pm)
        {
            ImGui::Separator();
            section_label("OUTFITS", s_padFocusOutfits);

            const int count = pm.preset_count();
            if (count <= 0)
                ui_text_disabled("No saved outfits yet. Save New creates one from the current draft.");
            else
            {
                s_outfitNav = std::clamp(s_outfitNav, 0, count - 1);
                ImGui::BeginChild("##outfit_list", ImVec2(0.0f, 145.0f), true);
                const auto &presets = pm.presets();
                for (int i = 0; i < count; ++i)
                {
                    const bool active = i == pm.active_preset_index();
                    const bool padFocus = s_padFocusOutfits && i == s_outfitNav;
                    if (padFocus)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Header, kFocus);
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kFocusHover);
                    }
                    char label[192];
                    std::snprintf(label, sizeof(label), "%02d   %s%s##outfit_%d", i + 1,
                                  presets[static_cast<std::size_t>(i)].name.c_str(), active ? "   active" : "", i);
                    if (ImGui::Selectable(label, active || padFocus, 0, ImVec2(0.0f, 38.0f)))
                    {
                        s_outfitNav = i;
                        preview_outfit(pm, i);
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                        rename_outfit(pm, i);
                    if (padFocus)
                        ImGui::PopStyleColor(2);
                }
                ImGui::EndChild();
            }

            const float avail = ImGui::GetContentRegionAvail().x;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float bw = std::max(98.0f, (avail - gap * 2.0f) / 3.0f);
            for (int i = 0; i < static_cast<int>(OutfitAction::Count); ++i)
            {
                if (i == 3)
                    ImGui::NewLine();
                else if (i != 0)
                    ImGui::SameLine();
                const auto action = static_cast<OutfitAction>(i);
                const bool focus = s_padFocusOutfits && s_outfitActionNav == i;
                if (focus)
                    ImGui::PushStyleColor(ImGuiCol_Button, kGoldDim);
                char label[96];
                std::snprintf(label, sizeof(label), "%s##outfit_action_%d", outfit_action_name(action), i);
                if (ImGui::Button(label, ImVec2(bw, 38.0f)))
                    run_outfit_action(pm, action);
                if (focus)
                    ImGui::PopStyleColor();
            }
        }

        void draw_controller_legend(const PadState &pad)
        {
            ImGui::Separator();
            if (!pad.connected)
            {
                section_label("CONTROLS");
                ui_text_disabled("Mouse hover previews. Click pins. Apply saves.");
                return;
            }

            section_label(s_padFocusOutfits ? "XBOX  -  OUTFITS" : "XBOX  -  CATALOGUE", true);
            if (s_padFocusOutfits)
            {
                ui_text("D-pad Up/Down  Outfit");
                ui_text("D-pad Left/Right  Action");
                ui_text("A  Run action     X  Apply");
                ui_text("View/B  Back to catalogue");
            }
            else
            {
                ui_text("LB/RB  Slot       D-pad  Browse");
                ui_text("A  Pin/Palette    X  Apply");
                ui_text("Y  Appearance/Colour");
                ui_text("View  Outfits     RS  Catalogue");
                ui_text("B  Cancel / Close");
            }
        }

        void draw_left_panel(PresetManager &pm, const PadState &pad, const ImVec2 &size)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
            ImGui::BeginChild("##wardrobe_left", size, true);
            draw_character_header(pm);
            ImGui::Separator();

            const float reserve = pad.connected ? 495.0f : 420.0f;
            const float slotH = std::max(300.0f, ImGui::GetContentRegionAvail().y - reserve);
            ImGui::BeginChild("##slot_selector_scroll", ImVec2(0.0f, slotH), false);
            draw_slot_selector();
            ImGui::EndChild();
            draw_outfits(pm);
            draw_controller_legend(pad);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        void draw_filters_popup(SlotUIState &ui)
        {
            if (ImGui::BeginPopup("Wardrobe filters###wardrobe_filters"))
            {
                ImGui::Checkbox("Safe only", &ui.hideIncompatible);
                ImGui::Checkbox("Hide variants", &ui.hideVariants);
                ImGui::Checkbox("Hide cross-body", &ui.hideBodyMismatch);
                ui_text_disabled("Slot matching is always exact in Wardrobe mode.");
                ImGui::EndPopup();
            }
        }

        void draw_catalogue_toggle()
        {
            if (s_spoilerPopupRequest)
            {
                s_spoilerPopupRequest = false;
                s_spoilerPopupActive = true;
                ImGui::OpenPopup("Show all catalogue?###wardrobe_spoiler_confirm");
            }

            if (!s_showAllCatalogue)
            {
                if (ImGui::Button("Catalogue: Discovered##catalogue_mode", ImVec2(260.0f, 38.0f)))
                {
                    s_spoilerPopupRequest = true;
                }
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
                if (ImGui::Button("Catalogue: ALL - SPOILERS##catalogue_mode", ImVec2(285.0f, 38.0f)))
                    s_showAllCatalogue = false;
                ImGui::PopStyleColor();
            }

            if (ImGui::BeginPopupModal("Show all catalogue?###wardrobe_spoiler_confirm", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                s_spoilerPopupActive = true;
                section_label("UNRESTRICTED CATALOGUE");
                ui_text("Show equipment that has not been discovered in this playthrough?");
                ui_text_disabled("This is deliberately spoiler-protected. Browsing here never adds discoveries.");

                if (s_spoilerAccept)
                {
                    s_spoilerAccept = false;
                    s_showAllCatalogue = true;
                    s_spoilerPopupActive = false;
                    ImGui::CloseCurrentPopup();
                }
                else if (s_spoilerCancel)
                {
                    s_spoilerCancel = false;
                    s_spoilerPopupActive = false;
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    if (ImGui::Button("Show all - spoilers", ImVec2(230.0f, 42.0f)))
                    {
                        s_showAllCatalogue = true;
                        s_spoilerPopupActive = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Keep discovered only", ImVec2(250.0f, 42.0f)))
                    {
                        s_spoilerPopupActive = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ui_text_disabled("Controller: A confirm   B cancel");
                }
                ImGui::EndPopup();
            }
        }

        void handle_hover_candidate(std::size_t slot, const SlotMapping &candidate)
        {
            const std::uint16_t id = candidate.targetItemId;
            if (s_session.hoverPendingId != id || s_session.hoverPendingActive != candidate.active)
            {
                s_session.hoverPendingId = id;
                s_session.hoverPendingActive = candidate.active;
                s_session.hoverStartMs = now_ms();
                return;
            }
            if (s_session.hoverStartMs != 0 && now_ms() - s_session.hoverStartMs >= 150 &&
                !mapping_equal(candidate, s_session.draft[slot]))
                preview_mapping(slot, candidate);
        }

        bool draw_candidate_row(const CatalogueCandidate &candidate, int index, std::size_t slot, bool *outHovered,
                                bool *outControllerFocused)
        {
            const bool draft = mapping_equal(s_session.draft[slot], candidate.mapping);
            const bool applied = mapping_equal(s_session.baseline[slot], candidate.mapping);
            const bool focused = s_padCatalogueFocus && !s_padFocusOutfits && index == s_catalogNav;

            if (focused)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, kFocus);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kFocusHover);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, kFocus);
            }
            else if (draft && !applied)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, kGoldDim);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kFocusHover);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, kGoldDim);
            }

            const ImVec2 rowPos = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            constexpr float rowH = 48.0f;
            std::string id = candidate.label + "##appearance_" + candidate.stableId;
            const bool clicked = ImGui::Selectable(id.c_str(), focused || draft, 0, ImVec2(0.0f, rowH));
            const bool hovered = ImGui::IsItemHovered();
            if (outHovered)
                *outHovered = hovered;
            if (outControllerFocused)
                *outControllerFocused = focused;

            // Compact state markers: no APPLIED/DRAFT words competing with the item name.
            auto *dl = ImGui::GetWindowDrawList();
            const ImVec2 marker(rowPos.x + rowW - 18.0f, rowPos.y + rowH * 0.5f);
            if (draft && !applied)
            {
                const float r = 6.0f;
                dl->AddQuadFilled(ImVec2(marker.x, marker.y - r), ImVec2(marker.x + r, marker.y),
                                  ImVec2(marker.x, marker.y + r), ImVec2(marker.x - r, marker.y),
                                  ImGui::ColorConvertFloat4ToU32(kGold));
            }
            else if (applied)
            {
                dl->AddCircleFilled(marker, 5.5f, ImGui::ColorConvertFloat4ToU32(kGold), 20);
            }

            if (focused || (draft && !applied))
                ImGui::PopStyleColor(3);

            if (hovered)
            {
                std::string tip = candidate.label;
                if (applied)
                    tip += "\nGold dot: applied when Wardrobe opened";
                if (draft && !applied)
                    tip += "\nGold diamond: current draft";
                ui_tooltip(tip.c_str());
            }
            return clicked;
        }

        void draw_appearance_list(PresetManager &pm, const ImVec2 &panelSize)
        {
            auto &registry = DiscoveryRegistry::instance();
            registry.ensure_ready();
            auto &ui = s_slotUI[s_selectedSlot];
            ui.exactFilter = true;

            const float searchW = std::max(360.0f, panelSize.x - 620.0f);
            ImGui::SetNextItemWidth(searchW);
            ImGui::InputTextWithHint("##wardrobe_search", "Search discovered appearances", ui.searchBuf, sizeof(ui.searchBuf));
            ImGui::SameLine();
            if (ImGui::Button("Filters##wardrobe_filters_btn", ImVec2(120.0f, 38.0f)))
                ImGui::OpenPopup("Wardrobe filters###wardrobe_filters");
            draw_filters_popup(ui);
            ImGui::SameLine();
            draw_catalogue_toggle();

            auto candidates = build_catalogue_candidates(pm);
            s_lastVisibleCount = static_cast<int>(candidates.size());
            if (s_lastVisibleCount <= 0)
                s_catalogNav = 0;
            else
                s_catalogNav = std::clamp(s_catalogNav, 0, s_lastVisibleCount - 1);

            const auto slot = static_cast<TransmogSlot>(s_selectedSlot);
            const std::size_t realCount = candidates.size() >= 2 ? candidates.size() - 2 : 0;
            ui_text_disabled("%zu discovered appearance%s for %s%s", realCount, realCount == 1 ? "" : "s",
                             wardrobe_slot_name(slot), s_showAllCatalogue ? "  -  unrestricted" : "");

            ImGui::BeginChild("##appearance_list", ImVec2(0.0f, std::max(220.0f, panelSize.y - 185.0f)), true);
            bool anyMouseHover = false;
            bool anyControllerFocus = false;
            for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
            {
                bool hovered = false;
                bool focused = false;
                const bool clicked = draw_candidate_row(candidates[static_cast<std::size_t>(i)], i, s_selectedSlot,
                                                        &hovered, &focused);
                if (hovered)
                {
                    anyMouseHover = true;
                    s_catalogNav = i;
                    s_padCatalogueFocus = false;
                    handle_hover_candidate(s_selectedSlot, candidates[static_cast<std::size_t>(i)].mapping);
                }
                else if (focused)
                {
                    anyControllerFocus = true;
                    handle_hover_candidate(s_selectedSlot, candidates[static_cast<std::size_t>(i)].mapping);
                }
                if (clicked)
                    pin_draft_mapping(s_selectedSlot, candidates[static_cast<std::size_t>(i)].mapping);
            }
            if (!anyMouseHover && !anyControllerFocus && s_session.previewActive && s_session.previewSlot == s_selectedSlot)
                restore_hover_preview();
            ImGui::EndChild();
        }

        void draw_colour_panel(PresetManager &pm)
        {
            const auto slot = static_cast<TransmogSlot>(s_selectedSlot);
            section_label("COLOUR");
            if (!is_armor_slot(slot))
            {
                ui_text_disabled("Colour controls are currently available for armor slots only.");
                return;
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanelSoft);
            ImGui::BeginChild("##dye_card", ImVec2(0.0f, 145.0f), true);
            section_label("DYE PALETTE");
            ui_text_disabled("Game-compatible ARMOR_MOD colour and material palette.");
            ui_text("Open the palette and choose a dye channel, colour family and shade.");
            ImGui::PushID(static_cast<int>(s_selectedSlot));
            if (s_openDyeRequested)
            {
                pm.active_preset_mut_or_create();
                ImGui::OpenPopup("##dye_picker");
                s_openDyeRequested = false;
            }
            draw_dye_popup(s_selectedSlot);
            ImGui::PopID();
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanelSoft);
            ImGui::BeginChild("##extended_colour_card", ImVec2(0.0f, 105.0f), true);
            section_label("EXTENDED MATERIAL COLOUR");
            if (flag_color_override().load(std::memory_order_acquire))
                ui_text_colored(kGold, "Enabled");
            else
                ui_text_disabled("Off  -  experimental ColorOverride is disabled in the INI; restart required to enable it.");
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        void draw_right_panel(PresetManager &pm, const ImVec2 &size, const PadState &pad)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
            ImGui::BeginChild("##wardrobe_right", size, true);
            ui_text_colored(kIvory, "%s", wardrobe_slot_name(static_cast<TransmogSlot>(s_selectedSlot)));
            ImGui::SameLine();
            ui_text_disabled("APPEARANCE");
            ImGui::Spacing();

            const bool appearanceSelected = s_tab == WardrobeTab::Appearance;
            if (appearanceSelected)
                ImGui::PushStyleColor(ImGuiCol_Button, kGoldDim);
            if (ImGui::Button("Appearance##wardrobe_tab_appearance", ImVec2(175.0f, 42.0f)))
                s_tab = WardrobeTab::Appearance;
            if (appearanceSelected)
                ImGui::PopStyleColor();
            ImGui::SameLine();
            const bool colourSelected = s_tab == WardrobeTab::Colour;
            if (colourSelected)
                ImGui::PushStyleColor(ImGuiCol_Button, kGoldDim);
            if (ImGui::Button("Colour##wardrobe_tab_colour", ImVec2(140.0f, 42.0f)))
                s_tab = WardrobeTab::Colour;
            if (colourSelected)
                ImGui::PopStyleColor();
            ImGui::Separator();

            if (s_tab == WardrobeTab::Appearance)
                draw_appearance_list(pm, size);
            else
                draw_colour_panel(pm);

            ImGui::Separator();
            const bool dirty = session_dirty(pm);
            if (!dirty)
                ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.34f, 0.245f, 0.11f, 0.98f));
            if (ImGui::Button("Apply##wardrobe_apply", ImVec2(135.0f, 42.0f)) && dirty)
                apply_session(pm);
            ImGui::PopStyleColor();
            if (!dirty)
                ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel##wardrobe_cancel", ImVec2(135.0f, 42.0f)))
                cancel_session(pm);
            ImGui::SameLine();
            if (ImGui::Button("Reset slot##wardrobe_reset_slot", ImVec2(150.0f, 42.0f)))
                pin_draft_mapping(s_selectedSlot, SlotMapping{false, 0});
            ImGui::SameLine();
            if (dirty)
                ui_text_colored(kGold, "unapplied changes");
            else
                ui_text_disabled("saved");

            // Controller instructions intentionally live ONLY in the left panel. The right panel stays content-focused.
            (void)pad;
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        void handle_controller(PresetManager &pm, const PadState &pad)
        {
            if (!pad.connected || pad.pressed == 0)
                return;
            if (!s_loggedPad)
            {
                s_loggedPad = true;
                DMK::Logger::get_instance().info("[wardrobe-input] XInput controller detected; v3 navigation active");
            }

            if (s_spoilerPopupActive)
            {
                if (pad.pressed & XINPUT_GAMEPAD_A)
                    s_spoilerAccept = true;
                if (pad.pressed & XINPUT_GAMEPAD_B)
                    s_spoilerCancel = true;
                return;
            }

            if (s_renameOpen)
            {
                if (pad.pressed & XINPUT_GAMEPAD_B)
                    s_renameCancelRequested = true;
                return;
            }

            if (pad.pressed & XINPUT_GAMEPAD_BACK)
            {
                restore_hover_preview();
                s_padFocusOutfits = !s_padFocusOutfits;
                s_padCatalogueFocus = !s_padFocusOutfits;
                s_outfitActionNav = static_cast<int>(OutfitAction::Load);
            }

            if (!s_padFocusOutfits && (pad.pressed & XINPUT_GAMEPAD_RIGHT_THUMB))
            {
                if (s_showAllCatalogue)
                    s_showAllCatalogue = false;
                else
                    s_spoilerPopupRequest = true;
            }

            if (pad.pressed & XINPUT_GAMEPAD_Y)
            {
                s_tab = s_tab == WardrobeTab::Appearance ? WardrobeTab::Colour : WardrobeTab::Appearance;
                restore_hover_preview();
            }

            if (!s_padFocusOutfits)
            {
                if (pad.pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
                    cycle_slot(-1);
                if (pad.pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                    cycle_slot(+1);

                if (s_tab == WardrobeTab::Appearance)
                {
                    const auto candidates = build_catalogue_candidates(pm);
                    const int count = static_cast<int>(candidates.size());
                    if (count > 0)
                    {
                        if (pad.pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT))
                        {
                            restore_hover_preview();
                            s_catalogNav = (s_catalogNav + count - 1) % count;
                            s_padCatalogueFocus = true;
                        }
                        if (pad.pressed & (XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT))
                        {
                            restore_hover_preview();
                            s_catalogNav = (s_catalogNav + 1) % count;
                            s_padCatalogueFocus = true;
                        }
                        if (pad.pressed & XINPUT_GAMEPAD_A)
                        {
                            s_catalogNav = std::clamp(s_catalogNav, 0, count - 1);
                            pin_draft_mapping(s_selectedSlot, candidates[static_cast<std::size_t>(s_catalogNav)].mapping);
                            s_padCatalogueFocus = true;
                            DMK::Logger::get_instance().info("[wardrobe-input] A pinned catalogue index={} label='{}'",
                                                             s_catalogNav, candidates[static_cast<std::size_t>(s_catalogNav)].label);
                        }
                    }
                }
                else if (pad.pressed & XINPUT_GAMEPAD_A)
                {
                    s_openDyeRequested = true;
                }
            }
            else
            {
                const int count = pm.preset_count();
                if (count > 0)
                {
                    if (pad.pressed & XINPUT_GAMEPAD_DPAD_UP)
                        s_outfitNav = (s_outfitNav + count - 1) % count;
                    if (pad.pressed & XINPUT_GAMEPAD_DPAD_DOWN)
                        s_outfitNav = (s_outfitNav + 1) % count;
                }
                const int actionCount = static_cast<int>(OutfitAction::Count);
                if (pad.pressed & XINPUT_GAMEPAD_DPAD_LEFT)
                    s_outfitActionNav = (s_outfitActionNav + actionCount - 1) % actionCount;
                if (pad.pressed & XINPUT_GAMEPAD_DPAD_RIGHT)
                    s_outfitActionNav = (s_outfitActionNav + 1) % actionCount;
                if (pad.pressed & XINPUT_GAMEPAD_A)
                    run_outfit_action(pm, static_cast<OutfitAction>(s_outfitActionNav));
            }

            if (pad.pressed & XINPUT_GAMEPAD_X)
                if (session_dirty(pm))
                    apply_session(pm);

            if (pad.pressed & XINPUT_GAMEPAD_B)
            {
                if (s_padFocusOutfits)
                {
                    s_padFocusOutfits = false;
                    s_padCatalogueFocus = true;
                }
                else
                {
                    cancel_session(pm, false);
                    toggle_overlay_visible();
                }
            }
        }

        void draw_top_controls(PresetManager &pm, const PadState &pad)
        {
            ui_text_colored(kGold, "WARDROBE");
            ImGui::SameLine();
            ui_text_disabled("appearance configuration");
            const auto avail = ImGui::GetContentRegionAvail().x;
            if (avail > 760.0f)
                ImGui::SameLine(ImGui::GetWindowWidth() - 610.0f);
            if (ImGui::Button(s_showAllCatalogue ? "ALL CATALOGUE - SPOILERS##top_catalogue" :
                                                      "DISCOVERED ONLY##top_catalogue",
                              ImVec2(255.0f, 38.0f)))
            {
                if (s_showAllCatalogue)
                    s_showAllCatalogue = false;
                else
                    s_spoilerPopupRequest = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Diagnostics##wardrobe_diag", ImVec2(145.0f, 38.0f)))
                log_diagnostics(pm, pad);
            ImGui::SameLine();
            ui_text_disabled("FREQEE v3");
        }

        void draw_center_panel(const ImVec2 &size)
        {
            // Intentionally transparent. This area belongs to the live character and later inspection-camera work.
            ImGui::BeginChild("##wardrobe_viewport", size, false);
            ImGui::EndChild();
        }

        void draw_wardrobe_content()
        {
            ColorOverride::Reinit::tick();
            auto &pm = PresetManager::instance();
            pm.reseed_unresolved_persisted_swatches();
            DiscoveryRegistry::instance().ensure_ready();
            if (is_world_ready())
                Wardrobe::IconProbe::tick_once_world_ready();

            const auto now = now_ms();
            if (!s_session.active || (s_session.lastDrawMs != 0 && now - s_session.lastDrawMs > 500))
            {
                if (s_session.active)
                    cancel_session(pm, false);
                begin_session(pm);
            }
            s_session.lastDrawMs = now;

            const PadState pad = poll_gamepad();
            handle_controller(pm, pad);
            draw_top_controls(pm, pad);
            ImGui::Separator();

            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float gap = std::clamp(avail.x * 0.009f, 18.0f, 34.0f);
            const float leftW = std::clamp(avail.x * 0.13f, 520.0f, 680.0f);
            const float rightW = std::clamp(avail.x * 0.28f, 940.0f, 1450.0f);
            const float panelH = avail.y;

            draw_left_panel(pm, pad, ImVec2(leftW, panelH));
            ImGui::SameLine(0.0f, gap);
            const float centerW = std::max(240.0f, avail.x - leftW - rightW - gap * 2.0f);
            draw_center_panel(ImVec2(centerW, panelH));
            ImGui::SameLine(0.0f, gap);
            draw_right_panel(pm, ImVec2(rightW, panelH), pad);
            draw_rename_popup(pm);

            if (!s_loggedLayout)
            {
                s_loggedLayout = true;
                DMK::Logger::get_instance().info("[wardrobe-ui] v3 layout: available={}x{} left={} center={} right={} gap={}",
                    static_cast<int>(avail.x), static_cast<int>(avail.y), static_cast<int>(leftW),
                    static_cast<int>(centerW), static_cast<int>(rightW), static_cast<int>(gap));
            }
        }
    } // anonymous namespace

    void draw_overlay()
    {
        auto &io = ImGui::GetIO();
        const ImVec2 display = io.DisplaySize;
        const float marginX = std::clamp(display.x * 0.022f, 28.0f, 68.0f);
        const float marginY = std::clamp(display.y * 0.033f, 24.0f, 48.0f);

        ImGui::SetNextWindowPos(ImVec2(marginX, marginY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::max(1100.0f, display.x - marginX * 2.0f),
                                       std::max(700.0f, display.y - marginY * 2.0f)), ImGuiCond_Always);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
        ImGui::PushStyleColor(ImGuiCol_Text, kIvory);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, kMuted);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.075f, 0.082f, 0.079f, 0.97f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.17f, 0.105f, 0.99f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.29f, 0.22f, 0.105f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, kPanelSoft);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.19f, 0.16f, 0.10f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, kGoldDim);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(13.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(11.0f, 9.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(9.0f, 7.0f));

        if (ImGui::Begin("Wardrobe###TransmogMain", nullptr, flags))
        {
            s_standaloneMode = true;
            draw_wardrobe_content();
        }
        ImGui::End();
        ImGui::PopStyleVar(8);
        ImGui::PopStyleColor(9);
    }

    static HMODULE s_reshadeModule = nullptr;
    static bool s_reshadeActive = false;

    static void draw_reshade_overlay(reshade::api::effect_runtime *)
    {
        s_standaloneMode = false;
        draw_wardrobe_content();
    }

    bool init_reshade_overlay(HMODULE hModule)
    {
        if (!reshade::register_addon(hModule))
            return false;
        reshade::register_overlay("Wardrobe", &draw_reshade_overlay);
        s_reshadeModule = hModule;
        s_reshadeActive = true;
        return true;
    }

    void shutdown_reshade_overlay()
    {
        if (!s_reshadeActive)
            return;
        reshade::unregister_overlay("Wardrobe", &draw_reshade_overlay);
        reshade::unregister_addon(s_reshadeModule);
        s_reshadeActive = false;
        s_reshadeModule = nullptr;
    }

    bool is_reshade_overlay_active()
    {
        return s_reshadeActive;
    }
} // namespace Transmog
