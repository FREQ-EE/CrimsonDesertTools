// overlay_ui.cpp -- FREQEE wardrobe UI.
//
// Phase 3A deliberately keeps the proven transmog / carrier / dye machinery untouched and replaces only the user-facing
// presentation + session semantics. The wardrobe is transactional:
//
//   hover -> temporary visual preview
//   click -> pin into the in-memory draft
//   Apply -> persist the draft into the active Outfit (preset)
//   Cancel -> restore the outfit that was active when the wardrobe opened
//
// Undiscovered catalogue entries are removed before rendering by Wardrobe::DiscoveryRegistry. The unrestricted catalog
// exists only behind an explicit spoiler confirmation and is session-only.

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
#include "wardrobe_discovery.hpp"

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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace Transmog
{
    namespace
    {
        using Wardrobe::DiscoveryRegistry;

        constexpr ImVec4 kGold{0.83f, 0.64f, 0.34f, 1.0f};
        constexpr ImVec4 kGoldDim{0.45f, 0.34f, 0.19f, 0.78f};
        constexpr ImVec4 kPanel{0.055f, 0.064f, 0.064f, 0.91f};
        constexpr ImVec4 kPanelSoft{0.043f, 0.050f, 0.050f, 0.84f};
        constexpr ImVec4 kBorder{0.72f, 0.69f, 0.61f, 0.28f};
        constexpr ImVec4 kMuted{0.68f, 0.65f, 0.59f, 1.0f};
        constexpr ImVec4 kDanger{0.85f, 0.53f, 0.46f, 1.0f};

        enum class WardrobeTab
        {
            Appearance,
            Colour,
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

        WardrobeSession s_session{};
        WardrobeTab s_tab = WardrobeTab::Appearance;
        std::size_t s_selectedSlot = 0;
        bool s_showAllCatalogue = false;
        int s_catalogNav = 0;
        int s_outfitNav = 0;
        int s_lastVisibleCount = 0;
        bool s_padFocusOutfits = false;
        bool s_loggedLayout = false;
        bool s_loggedPad = false;
        char s_renameBuf[64]{};
        int s_renameIndex = -1;
        bool s_renameOpen = false;

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
            case TransmogSlot::Helm:
                return "Head";
            case TransmogSlot::Chest:
                return "Chest";
            case TransmogSlot::Cloak:
                return "Back";
            case TransmogSlot::Gloves:
                return "Hands";
            case TransmogSlot::Boots:
                return "Boots";
            case TransmogSlot::Earring1:
                return "Earring I";
            case TransmogSlot::Earring2:
                return "Earring II";
            case TransmogSlot::Necklace:
                return "Necklace";
            case TransmogSlot::Ring1:
                return "Ring I";
            case TransmogSlot::Ring2:
                return "Ring II";
            case TransmogSlot::Lantern:
                return "Lantern";
            case TransmogSlot::Glasses:
                return "Glasses";
            case TransmogSlot::Mask:
                return "Mask";
            case TransmogSlot::Backpack:
                return "Backpack";
            case TransmogSlot::Bracelet:
                return "Bracelet";
            case TransmogSlot::MainHand:
                return "Main Hand";
            case TransmogSlot::OffHand:
                return "Off Hand";
            case TransmogSlot::Ranged:
                return "Ranged";
            case TransmogSlot::SubWeapon:
                return "Sub Weapon";
            case TransmogSlot::TwoHandWeapon:
                return "Two Hand";
            case TransmogSlot::Tool:
                return "Tool";
            case TransmogSlot::OffHand2:
                return "Off Hand II";
            case TransmogSlot::Ranged2:
                return "Ranged II";
            default:
                return "Appearance";
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
            if (mapping.active && mapping.targetItemId != 0)
                DiscoveryRegistry::instance().mark_id(mapping.targetItemId, "wardrobe-pick");
        }

        void begin_session(PresetManager &pm)
        {
            ensure_selected_slot();
            DiscoveryRegistry::instance().ensure_ready();
            s_session.active = true;
            s_session.baselinePreset = pm.active_preset_index();
            s_session.baseline = slot_mappings();
            s_session.draft = s_session.baseline;
            s_session.previewActive = false;
            s_session.hoverStartMs = 0;
            s_catalogNav = 0;
            s_outfitNav = std::max(0, pm.active_preset_index());
            DMK::Logger::get_instance().info(
                "[wardrobe] session begin: character='{}' outfit={} discovered={}", pm.editing_character(),
                pm.active_preset_index(), DiscoveryRegistry::instance().size());
        }

        [[nodiscard]] bool session_dirty(PresetManager &pm)
        {
            if (!mapping_arrays_equal(s_session.draft, s_session.baseline))
                return true;
            if (pm.active_preset_index() != s_session.baselinePreset)
                return true;
            if (dye_dirty().load(std::memory_order_acquire))
                return true;
            return false;
        }

        void cancel_session(PresetManager &pm, bool startFresh = true)
        {
            restore_hover_preview();

            if (pm.preset_count() > 0)
                pm.set_active_preset(s_session.baselinePreset);

            // Baseline is the exact mapping snapshot at wardrobe-open, so restore it after set_active_preset() even if
            // a legacy preset contains a disabled row that apply_to_state normalises differently.
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

            // This is the single persistence boundary. It captures slot mappings, dye records and ColorOverride
            // swatches, and PresetManager::replace_current_from_state() writes presets.json.
            pm.replace_current_from_state();
            DiscoveryRegistry::instance().ensure_ready();

            s_session.baselinePreset = pm.active_preset_index();
            s_session.baseline = slot_mappings();
            s_session.draft = s_session.baseline;
            s_session.previewActive = false;
            DMK::Logger::get_instance().info("[wardrobe] applied and saved: character='{}' outfit={}",
                                             pm.editing_character(), pm.active_preset_index());
        }

        [[nodiscard]] ItemNameTable::BodyKind editing_body_kind(PresetManager &pm)
        {
            using BK = ItemNameTable::BodyKind;
            const std::string ov = pm.body_kind_of(pm.editing_character());
            if (ov == "Male")
                return BK::Male;
            if (ov == "Female")
                return BK::Female;
            if (ov == "Both")
                return BK::Both;
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

        [[nodiscard]] std::string display_for_item(std::uint16_t id)
        {
            if (id == 0)
                return "Hidden";
            const auto &table = ItemNameTable::instance();
            const auto internal = table.name_of(id);
            if (internal.empty())
                return "Unknown appearance";
            const auto display = table.display_name_of(internal);
            return display.empty() ? internal : display;
        }

        [[nodiscard]] bool mapping_is_candidate(const SlotMapping &m, const SlotMapping &candidate) noexcept
        {
            return mapping_equal(m, candidate);
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
            if (direction > 0)
                pos = (pos + 1) % slots.size();
            else
                pos = (pos + slots.size() - 1) % slots.size();
            s_selectedSlot = slots[pos];
            s_catalogNav = 0;
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
            DiscoveryRegistry::instance().ensure_ready();
            DMK::Logger::get_instance().info("[wardrobe] outfit preview index={} name='{}'", index,
                                             pm.active_preset() ? pm.active_preset()->name : std::string{});
        }

        void rename_outfit(PresetManager &pm, int index)
        {
            if (index < 0 || index >= pm.preset_count())
                return;
            const auto &presets = pm.presets();
            std::snprintf(s_renameBuf, sizeof(s_renameBuf), "%s",
                          presets[static_cast<std::size_t>(index)].name.c_str());
            s_renameIndex = index;
            s_renameOpen = true;
            ImGui::OpenPopup("Rename outfit###wardrobe_rename");
        }

        void draw_rename_popup(PresetManager &pm)
        {
            if (!s_renameOpen)
                return;
            if (ImGui::BeginPopupModal("Rename outfit###wardrobe_rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::SetNextItemWidth(300.0f);
                const bool enter = ImGui::InputText("##outfit_name", s_renameBuf, sizeof(s_renameBuf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue, nullptr, nullptr);
                if (enter || ImGui::Button("Save name", ImVec2(120.0f, 0.0f)))
                {
                    if (s_renameIndex >= 0 && s_renameIndex < pm.preset_count())
                    {
                        const int restore = pm.active_preset_index();
                        pm.set_active_preset(s_renameIndex);
                        if (auto *p = pm.active_preset_mut())
                            p->name = s_renameBuf;
                        pm.set_active_preset(restore);
                        pm.save();
                    }
                    s_renameOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##rename", ImVec2(90.0f, 0.0f)))
                {
                    s_renameOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        void log_diagnostics(PresetManager &pm, const PadState &pad)
        {
            const auto &io = ImGui::GetIO();
            DMK::Logger::get_instance().info(
                "[wardrobe-diag] display={}x{} standalone={} char='{}' controlled='{}' preset={}/{} slot={} "
                "discovered={} visible={} showAll={} colorOverride={} gamepad={} dirty={}",
                static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y), s_standaloneMode,
                pm.editing_character(), pm.active_character(), pm.active_preset_index(), pm.preset_count(),
                wardrobe_slot_name(static_cast<TransmogSlot>(s_selectedSlot)), DiscoveryRegistry::instance().size(),
                s_lastVisibleCount, s_showAllCatalogue,
                flag_color_override().load(std::memory_order_relaxed), pad.connected, session_dirty(pm));
        }

        void draw_character_header(PresetManager &pm)
        {
            if (ImGui::Button("<##character_prev", ImVec2(30.0f, 30.0f)))
                switch_character(pm, -1);
            ImGui::SameLine();
            ui_text("%s", pm.editing_character().c_str());
            ImGui::SameLine();
            if (ImGui::Button(">##character_next", ImVec2(30.0f, 30.0f)))
                switch_character(pm, +1);

            ui_text_disabled("Character");
            ImGui::SameLine();
            const auto body = pm.body_kind_of(pm.editing_character());
            char bodyLabel[64];
            std::snprintf(bodyLabel, sizeof(bodyLabel), "Body: %s##body_cycle", body.c_str());
            if (ImGui::SmallButton(bodyLabel))
                cycle_body_filter(pm);
            if (ImGui::IsItemHovered())
                ui_tooltip("Picker body filter only. Auto follows the game's normal protagonist body type.");

            if (pm.editing_pinned())
            {
                ImGui::SameLine();
                ui_text_colored(kGold, "editing selected character");
            }
        }

        void draw_slot_selector()
        {
            const auto slots = enabled_slots();
            ui_text_disabled("APPEARANCE SLOT");
            const float avail = ImGui::GetContentRegionAvail().x;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float width = std::max(80.0f, (avail - gap) * 0.5f);
            int col = 0;
            for (const auto idx : slots)
            {
                if (col == 1)
                    ImGui::SameLine();
                const auto slot = static_cast<TransmogSlot>(idx);
                const bool selected = idx == s_selectedSlot;
                if (selected)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kGoldDim.x, kGoldDim.y, kGoldDim.z, 0.62f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kGoldDim.x + 0.08f, kGoldDim.y + 0.08f,
                                                                         kGoldDim.z + 0.05f, 0.78f));
                }
                char label[96];
                std::snprintf(label, sizeof(label), "%s##slot_%zu", wardrobe_slot_name(slot), idx);
                if (ImGui::Button(label, ImVec2(width, 36.0f)))
                {
                    restore_hover_preview();
                    s_selectedSlot = idx;
                    s_catalogNav = 0;
                }
                if (selected)
                    ImGui::PopStyleColor(2);
                col = 1 - col;
            }
        }

        void draw_outfits(PresetManager &pm)
        {
            ImGui::Separator();
            ui_text("Outfits");
            ImGui::SameLine();
            if (ImGui::SmallButton("Save current as new##outfit_save_new"))
            {
                restore_hover_preview();
                slot_mappings() = s_session.draft;
                pm.save_as_new_from_state();
                s_session.baselinePreset = pm.active_preset_index();
                s_session.baseline = slot_mappings();
                s_session.draft = s_session.baseline;
                s_outfitNav = pm.active_preset_index();
            }

            const int count = pm.preset_count();
            if (count <= 0)
            {
                ui_text_disabled("No saved outfits yet.");
                return;
            }

            ImGui::BeginChild("##outfit_list", ImVec2(0.0f, 150.0f), true);
            const auto &presets = pm.presets();
            for (int i = 0; i < count; ++i)
            {
                const bool active = i == pm.active_preset_index();
                char label[160];
                std::snprintf(label, sizeof(label), "%02d  %s%s##outfit_%d", i + 1,
                              presets[static_cast<std::size_t>(i)].name.c_str(), active ? "  *" : "", i);
                if (ImGui::Selectable(label, active || (s_padFocusOutfits && i == s_outfitNav)))
                    preview_outfit(pm, i);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    rename_outfit(pm, i);
            }
            ImGui::EndChild();

            if (ImGui::SmallButton("Rename##outfit_rename"))
                rename_outfit(pm, pm.active_preset_index());
            ImGui::SameLine();
            if (ImGui::SmallButton("Duplicate##outfit_duplicate"))
            {
                pm.duplicate_current();
                manual_apply();
                s_session.baselinePreset = pm.active_preset_index();
                s_session.baseline = slot_mappings();
                s_session.draft = s_session.baseline;
                s_outfitNav = pm.active_preset_index();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete##outfit_delete") && pm.preset_count() > 0)
            {
                pm.remove_current();
                if (pm.preset_count() > 0)
                    manual_apply();
                else
                    manual_clear();
                begin_session(pm);
            }
        }

        void draw_left_panel(PresetManager &pm, const ImVec2 &size)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
            ImGui::BeginChild("##wardrobe_left", size, true);
            draw_character_header(pm);
            ImGui::Separator();

            const float outfitReserve = 245.0f;
            const float slotH = std::max(180.0f, ImGui::GetContentRegionAvail().y - outfitReserve);
            ImGui::BeginChild("##slot_selector_scroll", ImVec2(0.0f, slotH), false);
            draw_slot_selector();
            ImGui::EndChild();
            draw_outfits(pm);
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
            if (!s_showAllCatalogue)
            {
                if (ImGui::SmallButton("Catalogue: Discovered##catalogue_mode"))
                    ImGui::OpenPopup("Show all catalogue?###wardrobe_spoiler_confirm");
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
                if (ImGui::SmallButton("Catalogue: ALL - SPOILERS##catalogue_mode"))
                    s_showAllCatalogue = false;
                ImGui::PopStyleColor();
            }

            if (ImGui::BeginPopupModal("Show all catalogue?###wardrobe_spoiler_confirm", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ui_text("Show every catalogue appearance?");
                ui_text_disabled("This reveals equipment names and appearances that have not been discovered in play.");
                if (ImGui::Button("Show all - spoilers", ImVec2(160.0f, 0.0f)))
                {
                    s_showAllCatalogue = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep discovered only", ImVec2(170.0f, 0.0f)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        void handle_hover_candidate(std::size_t slot, const SlotMapping &candidate, bool hovered)
        {
            if (!hovered)
                return;

            // Keep the exact clicked draft stable while the pointer auditions an alternative. 150 ms is short enough to
            // feel immediate, but long enough that crossing a row of tiles does not schedule a re-equip for every tile.
            const std::uint16_t id = candidate.targetItemId;
            if (s_session.hoverPendingId != id || s_session.hoverPendingActive != candidate.active)
            {
                s_session.hoverPendingId = id;
                s_session.hoverPendingActive = candidate.active;
                s_session.hoverStartMs = steady_ms();
                return;
            }

            if (s_session.hoverStartMs != 0 && steady_ms() - s_session.hoverStartMs >= 150 &&
                !mapping_equal(candidate, s_session.draft[slot]))
            {
                preview_mapping(slot, candidate);
            }
        }

        bool draw_appearance_tile(const char *visibleName, const char *stableId, const SlotMapping &candidate,
                                  std::size_t slot, const ImVec2 &size, bool *outHovered)
        {
            const bool draft = mapping_is_candidate(s_session.draft[slot], candidate);
            const bool baseline = mapping_is_candidate(s_session.baseline[slot], candidate);

            if (draft)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.23f, 0.13f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.39f, 0.30f, 0.17f, 0.96f));
                ImGui::PushStyleColor(ImGuiCol_Border, kGold);
            }
            else if (baseline)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.15f, 0.14f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.21f, 0.19f, 0.96f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.63f, 0.59f, 0.50f, 0.65f));
            }

            char label[256];
            std::snprintf(label, sizeof(label), "%s%s%s##appearance_%s", visibleName,
                          draft ? "\nDRAFT" : (baseline ? "\nAPPLIED" : ""), "", stableId);
            const bool clicked = ImGui::Button(label, size);
            const bool hovered = ImGui::IsItemHovered();
            if (outHovered)
                *outHovered = hovered;

            if (draft || baseline)
                ImGui::PopStyleColor(3);
            return clicked;
        }

        void draw_appearance_grid(PresetManager &pm, const ImVec2 &panelSize)
        {
            auto &registry = DiscoveryRegistry::instance();
            registry.ensure_ready();
            auto &ui = s_slotUI[s_selectedSlot];
            ui.exactFilter = true;

            ImGui::SetNextItemWidth(std::max(220.0f, panelSize.x - 220.0f));
            ImGui::InputTextWithHint("##wardrobe_search", "Search discovered appearances", ui.searchBuf,
                                     sizeof(ui.searchBuf));
            ImGui::SameLine();
            if (ImGui::SmallButton("Filters##wardrobe_filters_btn"))
                ImGui::OpenPopup("Wardrobe filters###wardrobe_filters");
            draw_filters_popup(ui);
            ImGui::SameLine();
            draw_catalogue_toggle();

            const auto slot = static_cast<TransmogSlot>(s_selectedSlot);
            const auto &table = ItemNameTable::instance();
            static thread_local std::vector<const ItemNameTable::Entry *> visible;
            visible.clear();
            if (table.ready())
            {
                for (const auto &e : table.sorted_entries())
                    if (entry_visible(e, slot, pm, ui))
                        visible.push_back(&e);
            }
            s_lastVisibleCount = static_cast<int>(visible.size()) + 2; // Equipped + Hidden

            ui_text_disabled("%zu appearance%s available for %s%s", visible.size(), visible.size() == 1 ? "" : "s",
                             wardrobe_slot_name(slot), s_showAllCatalogue ? " (unrestricted catalogue)" : "");

            const float tileGap = ImGui::GetStyle().ItemSpacing.x;
            const float avail = ImGui::GetContentRegionAvail().x;
            const int cols = avail >= 600.0f ? 4 : (avail >= 420.0f ? 3 : 2);
            const float tileW = std::max(105.0f, (avail - tileGap * static_cast<float>(cols - 1)) / cols);
            const ImVec2 tileSize(tileW, 82.0f);

            ImGui::BeginChild("##appearance_grid", ImVec2(0.0f, std::max(180.0f, panelSize.y - 165.0f)), false);
            bool anyHovered = false;
            int linearIndex = 0;

            auto emit = [&](const char *name, const char *stableId, const SlotMapping &candidate)
            {
                if ((linearIndex % cols) != 0)
                    ImGui::SameLine();
                bool hovered = false;
                const bool clicked = draw_appearance_tile(name, stableId, candidate, s_selectedSlot, tileSize, &hovered);
                if (hovered)
                {
                    anyHovered = true;
                    s_catalogNav = linearIndex;
                    handle_hover_candidate(s_selectedSlot, candidate, true);
                }
                if (clicked)
                    pin_draft_mapping(s_selectedSlot, candidate);
                ++linearIndex;
            };

            // "Equipped" = no transmog override. It is intentionally distinct from "Hidden" (active + itemId 0).
            emit("Equipped", "equipped", SlotMapping{false, 0});
            emit("Hidden", "hidden", SlotMapping{true, 0});

            for (const auto *e : visible)
            {
                std::string label = e->displayName.empty() ? e->name : e->displayName;
                if (label.size() > 34)
                {
                    label.resize(31);
                    label += "...";
                }
                char stable[96];
                std::snprintf(stable, sizeof(stable), "%04X_%s", e->id, e->name.c_str());
                emit(label.c_str(), stable, SlotMapping{true, e->id});
                if (s_showAllCatalogue && ImGui::IsItemHovered())
                {
                    char tip[320];
                    std::snprintf(tip, sizeof(tip), "%s\n%s\n0x%04X", e->displayName.c_str(), e->name.c_str(), e->id);
                    ui_tooltip(tip);
                }
            }

            // Mouse left the grid after a temporary hover preview: return to the clicked/pinned draft.
            if (!anyHovered && s_session.previewActive && s_session.previewSlot == s_selectedSlot)
                restore_hover_preview();

            ImGui::EndChild();
        }

        void draw_colour_panel()
        {
            const auto slot = static_cast<TransmogSlot>(s_selectedSlot);
            if (!is_armor_slot(slot))
            {
                ui_text("Colour");
                ui_text_disabled("The current dye-record and material override paths are available for armour slots only.");
                return;
            }

            ui_text("Colour");
            ui_text_disabled("Normal: game-style ARMOR_MOD dye records. Extended: per-material shader overrides.");
            if (!flag_color_override().load(std::memory_order_acquire))
            {
                ui_text_colored(kMuted, "Extended material colour is currently disabled in the INI.");
                ui_text_disabled("Set [Experimental] ColorOverride=true and restart to enable its engine hooks.");
            }
            ImGui::Separator();

            // The upstream dye popup is mature and already unifies the normal dye-record path with the experimental
            // per-material Color Override tab. Phase 3A embeds its launch point here instead of cloning that logic.
            ui_text("Edit %s", wardrobe_slot_name(slot));
            draw_dye_popup(s_selectedSlot);
        }

        void draw_right_panel(PresetManager &pm, const ImVec2 &size, const PadState &pad)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
            ImGui::BeginChild("##wardrobe_right", size, true);

            ui_text("%s", wardrobe_slot_name(static_cast<TransmogSlot>(s_selectedSlot)));
            ImGui::SameLine();
            ui_text_disabled("appearance");

            ImGui::Spacing();
            const bool appearanceSelected = s_tab == WardrobeTab::Appearance;
            if (appearanceSelected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kGoldDim.x, kGoldDim.y, kGoldDim.z, 0.64f));
            if (ImGui::Button("Appearance##wardrobe_tab_appearance", ImVec2(115.0f, 0.0f)))
                s_tab = WardrobeTab::Appearance;
            if (appearanceSelected)
                ImGui::PopStyleColor();
            ImGui::SameLine();
            const bool colourSelected = s_tab == WardrobeTab::Colour;
            if (colourSelected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kGoldDim.x, kGoldDim.y, kGoldDim.z, 0.64f));
            if (ImGui::Button("Colour##wardrobe_tab_colour", ImVec2(105.0f, 0.0f)))
                s_tab = WardrobeTab::Colour;
            if (colourSelected)
                ImGui::PopStyleColor();

            ImGui::Separator();
            if (s_tab == WardrobeTab::Appearance)
                draw_appearance_grid(pm, size);
            else
                draw_colour_panel();

            ImGui::Separator();
            const bool dirty = session_dirty(pm);
            if (!dirty)
                ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.23f, 0.12f, 0.94f));
            if (ImGui::Button("Apply##wardrobe_apply", ImVec2(105.0f, 34.0f)) && dirty)
                apply_session(pm);
            ImGui::PopStyleColor();
            if (!dirty)
                ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Cancel##wardrobe_cancel", ImVec2(105.0f, 34.0f)))
                cancel_session(pm);
            ImGui::SameLine();
            if (ImGui::Button("Reset slot##wardrobe_reset_slot", ImVec2(115.0f, 34.0f)))
                pin_draft_mapping(s_selectedSlot, SlotMapping{false, 0});

            ImGui::SameLine();
            if (dirty)
                ui_text_colored(kGold, "unapplied changes");
            else
                ui_text_disabled("no unapplied changes");

            if (pad.connected)
                ui_text_disabled("Controller: LB/RB slot - D-pad catalogue - A pin - X apply - Y tab - View outfits - B cancel/close");

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
                DMK::Logger::get_instance().info("[wardrobe-input] XInput controller detected; wardrobe navigation active");
            }

            if (pad.pressed & XINPUT_GAMEPAD_Y)
                s_tab = s_tab == WardrobeTab::Appearance ? WardrobeTab::Colour : WardrobeTab::Appearance;

            if (pad.pressed & XINPUT_GAMEPAD_BACK)
                s_padFocusOutfits = !s_padFocusOutfits;

            if (!s_padFocusOutfits)
            {
                if (pad.pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
                    cycle_slot(-1);
                if (pad.pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                    cycle_slot(+1);

                if (s_tab == WardrobeTab::Appearance)
                {
                    const int cols = 4; // conservative navigation stride; mouse layout may collapse to 3/2 columns.
                    if (pad.pressed & XINPUT_GAMEPAD_DPAD_LEFT)
                        s_catalogNav = std::max(0, s_catalogNav - 1);
                    if (pad.pressed & XINPUT_GAMEPAD_DPAD_RIGHT)
                        s_catalogNav = std::min(std::max(0, s_lastVisibleCount - 1), s_catalogNav + 1);
                    if (pad.pressed & XINPUT_GAMEPAD_DPAD_UP)
                        s_catalogNav = std::max(0, s_catalogNav - cols);
                    if (pad.pressed & XINPUT_GAMEPAD_DPAD_DOWN)
                        s_catalogNav = std::min(std::max(0, s_lastVisibleCount - 1), s_catalogNav + cols);
                    // A activation is completed inside the rendered catalogue once the nav row is materialised. For the
                    // first Phase 3A build we deliberately log the request instead of guessing an item identity from a
                    // stale filtered list. This is the one controller behaviour that needs runtime verification.
                    if (pad.pressed & XINPUT_GAMEPAD_A)
                        DMK::Logger::get_instance().debug("[wardrobe-input] A pressed on catalogue nav index={} (runtime activation probe)",
                                                         s_catalogNav);
                }
            }
            else
            {
                const int count = pm.preset_count();
                if (count > 0)
                {
                    if (pad.pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_LEFT_SHOULDER))
                        s_outfitNav = (s_outfitNav + count - 1) % count;
                    if (pad.pressed & (XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_RIGHT_SHOULDER))
                        s_outfitNav = (s_outfitNav + 1) % count;
                    if (pad.pressed & XINPUT_GAMEPAD_A)
                        preview_outfit(pm, s_outfitNav);
                }
            }

            if (pad.pressed & XINPUT_GAMEPAD_X)
            {
                if (session_dirty(pm))
                    apply_session(pm);
            }

            if (pad.pressed & XINPUT_GAMEPAD_B)
            {
                cancel_session(pm, false);
                toggle_overlay_visible();
            }
        }

        void draw_top_controls(PresetManager &pm, const PadState &pad)
        {
            ui_text("Wardrobe");
            ImGui::SameLine();
            ui_text_disabled("appearance configuration");

            const auto avail = ImGui::GetContentRegionAvail().x;
            if (avail > 520.0f)
                ImGui::SameLine(ImGui::GetWindowWidth() - 470.0f);
            draw_catalogue_toggle();
            ImGui::SameLine();
            if (ImGui::SmallButton("Diagnostics##wardrobe_diag"))
                log_diagnostics(pm, pad);
            ImGui::SameLine();
            ui_text_disabled("v%s", MOD_VERSION);
        }

        void draw_wardrobe_content()
        {
            if (s_standaloneMode)
                ImGui::GetIO().FontGlobalScale = s_uiScale;

            ColorOverride::Reinit::tick();
            auto &pm = PresetManager::instance();
            pm.reseed_unresolved_persisted_swatches();
            DiscoveryRegistry::instance().ensure_ready();

            const auto now = steady_ms();
            // draw_overlay is not called while the standalone window is hidden. A gap therefore marks a new wardrobe
            // visit. If the previous visit disappeared without Apply/Cancel (Escape / Home), restore its stored baseline
            // first so an abandoned draft never becomes the next session's starting truth.
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
            const float gap = std::clamp(avail.x * 0.012f, 12.0f, 32.0f);
            const float leftW = std::clamp(avail.x * 0.19f, 300.0f, 440.0f);
            const float rightW = std::clamp(avail.x * 0.30f, 470.0f, 720.0f);
            const float panelH = avail.y;

            draw_left_panel(pm, ImVec2(leftW, panelH));
            ImGui::SameLine(0.0f, gap);

            // Centre remains intentionally transparent: it is the live game character viewport. Inspection camera /
            // orbit controls arrive in Phase 3C; Phase 3A simply avoids obscuring the character.
            const float centerW = std::max(120.0f, avail.x - leftW - rightW - gap * 2.0f);
            ImGui::BeginChild("##wardrobe_viewport", ImVec2(centerW, panelH), false);
            ImGui::SetCursorPosY(std::max(0.0f, panelH - 32.0f));
            ui_text_disabled("Inspection camera / slot framing: Phase 3C");
            ImGui::EndChild();

            ImGui::SameLine(0.0f, gap);
            draw_right_panel(pm, ImVec2(rightW, panelH), pad);
            draw_rename_popup(pm);

            if (!s_loggedLayout)
            {
                s_loggedLayout = true;
                DMK::Logger::get_instance().info("[wardrobe-ui] layout: available={}x{} left={} center={} right={} gap={}",
                                                 static_cast<int>(avail.x), static_cast<int>(avail.y),
                                                 static_cast<int>(leftW), static_cast<int>(centerW),
                                                 static_cast<int>(rightW), static_cast<int>(gap));
            }
        }

    } // anonymous namespace

    void draw_overlay()
    {
        auto &io = ImGui::GetIO();
        const ImVec2 display = io.DisplaySize;
        const float marginX = std::clamp(display.x * 0.035f, 24.0f, 90.0f);
        const float marginY = std::clamp(display.y * 0.045f, 22.0f, 64.0f);

        ImGui::SetNextWindowPos(ImVec2(marginX, marginY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::max(800.0f, display.x - marginX * 2.0f),
                                       std::max(620.0f, display.y - marginY * 2.0f)),
                                 ImGuiCond_Always);

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.90f, 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, kMuted);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

        if (ImGui::Begin("Wardrobe###TransmogMain", nullptr, flags))
        {
            s_standaloneMode = true;
            draw_wardrobe_content();
        }
        ImGui::End();

        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(3);
    }

    // ReShade fallback. ReShade users get the same semantic controls inside the host tab, but without the full-screen
    // character viewport framing because the host owns the outer window geometry.
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
