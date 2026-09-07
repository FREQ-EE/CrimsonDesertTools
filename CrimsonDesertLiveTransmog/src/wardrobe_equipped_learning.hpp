#pragma once

#include "auth_table.hpp"
#include "shared_state.hpp"
#include "transmog_map.hpp"
#include "wardrobe_discovery.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace Transmog::Wardrobe
{
    /**
     * Learn the protagonist's CURRENTLY EQUIPPED real item appearances into the persistent discovery registry.
     *
     * This is deliberately narrower than an inventory scan: the authoritative equip table is already understood and
     * guarded by LiveTransmog, while inventory/storage ownership still needs a trustworthy game-side observation path.
     * It therefore gives us an immediate spoiler-safe improvement (anything genuinely worn is remembered forever)
     * without pretending that merely enumerating the game's 6813-item catalog means the player discovered it.
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

        const auto a1 = static_cast<std::uintptr_t>(player_a1().load(std::memory_order_acquire));
        if (a1 < 0x10000ULL)
            return;

        const auto container = DMKMemory::seh_read<std::uintptr_t>(a1 + AuthTable::k_containerPtrOffset).value_or(0);
        if (container < 0x10000ULL)
            return;
        const auto entries =
            DMKMemory::seh_read<std::uintptr_t>(container + AuthTable::k_containerArrayBaseOffset).value_or(0);
        const auto count =
            DMKMemory::seh_read<std::uint32_t>(container + AuthTable::k_containerCountOffset).value_or(0);
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
            const auto gate = DMKMemory::seh_read<std::uintptr_t>(base + AuthTable::k_entryGateOffset).value_or(0);
            if (gate == 0)
                continue;
            const auto liveId = DMKMemory::seh_read<std::uint16_t>(base + AuthTable::k_entryItemIdOffset).value_or(0);
            const auto gameSlot = DMKMemory::seh_read<std::int16_t>(base + AuthTable::k_entrySlotTagOffset).value_or(-1);
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
