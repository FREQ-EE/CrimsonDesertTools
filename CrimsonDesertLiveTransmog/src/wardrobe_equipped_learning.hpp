#pragma once

#include "auth_table.hpp"
#include "shared_state.hpp"
#include "transmog_map.hpp"
#include "wardrobe_discovery.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace Transmog::Wardrobe
{
    namespace Detail
    {
        template <typename T>
        [[nodiscard]] inline std::optional<T> safe_process_read(std::uintptr_t address) noexcept
        {
            if (address < 0x10000ULL)
                return std::nullopt;
            T value{};
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &value, sizeof(T), &got) ||
                got != sizeof(T))
                return std::nullopt;
            return value;
        }
    } // namespace Detail

    /**
     * Learn the protagonist's CURRENTLY EQUIPPED real item appearances into the persistent discovery registry.
     *
     * This is deliberately narrower than an inventory scan: the authoritative equip table is already understood by
     * LiveTransmog, while inventory/storage ownership still needs a trustworthy game-side observation path. It gives
     * us an immediate spoiler-safe improvement (anything genuinely worn is remembered forever) without pretending
     * that enumerating the game's 6813-item catalog means the player discovered it.
     *
     * When LT has a fake/carrier installed, last_applied_real_ids() is the source of truth for the underlying real
     * item. Otherwise the live auth-table item id is used. UI previews/catalogue picks can never enter this path.
     */
    inline void learn_currently_equipped() noexcept
    {
        auto &registry = DiscoveryRegistry::instance();
        registry.ensure_ready();
        if (!registry.ready())
            return;

        // Two verified owned armor rows were absent from v2 only because the canonical project record uses British
        // "Armour" while the game's display-name table spells them "Armor". Their stable internal ids come directly
        // from the shipped display-name TSV, so backfilling them does not reveal any unencountered equipment.
        static constexpr std::array<std::string_view, 2> kVerifiedStableBackfill = {
            "Doventry_Leather_Armor",
            "Douglas_Leather_Armor", // Blackwing Leather Armor
        };
        for (const auto name : kVerifiedStableBackfill)
            registry.mark_discovered(name, "verified play-history backfill");

        const auto a1 = static_cast<std::uintptr_t>(player_a1().load(std::memory_order_acquire));
        if (a1 < 0x10000ULL)
            return;

        const auto container = Detail::safe_process_read<std::uintptr_t>(a1 + AuthTable::k_containerPtrOffset).value_or(0);
        if (container < 0x10000ULL)
            return;
        const auto entries =
            Detail::safe_process_read<std::uintptr_t>(container + AuthTable::k_containerArrayBaseOffset).value_or(0);
        const auto count =
            Detail::safe_process_read<std::uint32_t>(container + AuthTable::k_containerCountOffset).value_or(0);
        if (entries < 0x10000ULL || count == 0 || count > 128)
            return;

        const auto &realIds = last_applied_real_ids();
        const auto &fakeIds = last_applied_ids();
        const auto &carrierIds = last_applied_carrier_ids();
        const auto &damaged = real_damaged();

        std::unordered_set<std::uint16_t> learnedThisPass;
        learnedThisPass.reserve(k_slotCount);

        for (std::uint32_t e = 0; e < count; ++e)
        {
            const auto base = AuthTable::entry_at(entries, e);
            const auto gate = Detail::safe_process_read<std::uintptr_t>(base + AuthTable::k_entryGateOffset).value_or(0);
            if (gate == 0)
                continue;
            const auto liveId = Detail::safe_process_read<std::uint16_t>(base + AuthTable::k_entryItemIdOffset).value_or(0);
            const auto gameSlot = Detail::safe_process_read<std::int16_t>(base + AuthTable::k_entrySlotTagOffset).value_or(-1);
            if (liveId == 0 || liveId == 0xFFFF)
                continue;

            const auto tmSlot = slot_from_game_slot(gameSlot);
            if (!tmSlot.has_value())
                continue;
            const auto idx = static_cast<std::size_t>(*tmSlot);
            if (idx >= k_slotCount)
                continue;

            // If this slot has been touched by LT, prefer its captured underlying real item rather than the fake
            // carrier currently present in the authoritative table. On a pristine slot realIds[idx] is normally zero,
            // so the live table is the legitimate equipped item.
            std::uint16_t realId = liveId;
            const bool ltTouched = fakeIds[idx] != 0 || carrierIds[idx] != 0 || damaged[idx];
            if (ltTouched && realIds[idx] != 0 && realIds[idx] != 0xFFFF)
                realId = realIds[idx];

            if (realId == 0 || realId == 0xFFFF || !learnedThisPass.insert(realId).second)
                continue;
            registry.mark_id(realId, "currently equipped real item");
        }
    }
} // namespace Transmog::Wardrobe
