#pragma once

#include "item_name_table.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>
#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Transmog::Wardrobe
{
    /**
     * Persistent spoiler-safe appearance registry.
     *
     * Stable internal item names are the persisted identity, never raw item ids. The ids may move between game
     * patches; ItemNameTable resolves each name against the current catalog every launch.
     *
     * Failure policy is deliberately CLOSED: if the JSON is missing or malformed, only the built-in canonical seed
     * plus the user's currently staged/applied mappings are visible. The registry never falls back to "show all".
     */
    class DiscoveryRegistry
    {
    public:
        static DiscoveryRegistry &instance()
        {
            static DiscoveryRegistry s;
            return s;
        }

        void ensure_ready()
        {
            std::scoped_lock lock(m_mutex);
            if (m_ready)
            {
                ingest_live_mappings_locked();
                return;
            }

            if (!ItemNameTable::instance().ready())
                return;

            m_path = runtime_path();
            load_locked();
            seed_canonical_locked();
            ingest_live_mappings_locked();
            m_ready = true;

            if (m_dirty)
                save_locked();

            DMK::Logger::get_instance().info("[wardrobe-discovery] ready: {} stable appearance name(s), file='{}'",
                                             m_names.size(), m_path.string());
        }

        [[nodiscard]] bool ready() const noexcept { return m_ready; }

        [[nodiscard]] bool contains(const ItemNameTable::Entry &entry) const
        {
            std::scoped_lock lock(m_mutex);
            return m_names.contains(lower(entry.name));
        }

        [[nodiscard]] bool contains_internal_name(std::string_view internalName) const
        {
            std::scoped_lock lock(m_mutex);
            return m_names.contains(lower(internalName));
        }

        void mark_discovered(std::string_view internalName, const char *reason = "runtime")
        {
            if (internalName.empty())
                return;
            std::scoped_lock lock(m_mutex);
            const auto key = lower(internalName);
            if (m_names.insert(key).second)
            {
                m_dirty = true;
                DMK::Logger::get_instance().info("[wardrobe-discovery] learned '{}' ({})", internalName,
                                                 reason ? reason : "runtime");
                if (m_ready)
                    save_locked();
            }
        }

        void mark_id(std::uint16_t id, const char *reason = "runtime")
        {
            const auto name = ItemNameTable::instance().name_of(id);
            if (!name.empty())
                mark_discovered(name, reason);
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            std::scoped_lock lock(m_mutex);
            return m_names.size();
        }

        [[nodiscard]] const std::filesystem::path &path() const noexcept { return m_path; }

    private:
        DiscoveryRegistry() = default;

        static std::string lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        static std::filesystem::path runtime_path()
        {
            // The validated release installation lives next to CrimsonDesert.exe in bin64. Using the executable path
            // avoids a non-portable function-pointer -> object-pointer cast merely to recover the ASI module handle.
            wchar_t buf[32768]{};
            const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            if (n == 0 || n >= std::size(buf))
                return std::filesystem::path("CrimsonDesertLiveTransmog_discovered.json");

            auto p = std::filesystem::path(std::wstring_view(buf, n));
            return p.parent_path() / L"CrimsonDesertLiveTransmog_discovered.json";
        }

        void load_locked()
        {
            m_names.clear();
            m_dirty = false;

            std::ifstream f(m_path);
            if (!f.is_open())
            {
                DMK::Logger::get_instance().info("[wardrobe-discovery] no registry yet; starting from canonical seed");
                return;
            }

            try
            {
                const auto root = nlohmann::json::parse(f);
                if (!root.is_object() || !root.contains("discovered") || !root["discovered"].is_array())
                {
                    DMK::Logger::get_instance().warning(
                        "[wardrobe-discovery] registry schema invalid; failing closed to canonical seed only");
                    return;
                }

                for (const auto &v : root["discovered"])
                {
                    if (!v.is_string())
                        continue;
                    const auto name = lower(v.get<std::string>());
                    if (!name.empty())
                        m_names.insert(name);
                }
            }
            catch (const std::exception &e)
            {
                DMK::Logger::get_instance().warning(
                    "[wardrobe-discovery] failed to parse registry ({}); failing closed to canonical seed only", e.what());
                m_names.clear();
            }
        }

        void save_locked()
        {
            if (m_path.empty())
                return;

            nlohmann::json root;
            root["version"] = 1;
            root["identity"] = "stable internal item names";
            root["discovered"] = nlohmann::json::array();

            std::vector<std::string> names(m_names.begin(), m_names.end());
            std::sort(names.begin(), names.end());
            for (const auto &name : names)
                root["discovered"].push_back(name);

            const auto tmp = m_path.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::trunc);
                if (!f.is_open())
                {
                    DMK::Logger::get_instance().warning("[wardrobe-discovery] cannot write temporary registry '{}'", tmp);
                    return;
                }
                f << root.dump(2) << '\n';
            }

            std::error_code ec;
            std::filesystem::rename(tmp, m_path, ec);
            if (ec)
            {
                // Windows rename does not replace an existing destination. Remove + retry while keeping the temp file
                // until the replacement succeeds.
                std::filesystem::remove(m_path, ec);
                ec.clear();
                std::filesystem::rename(tmp, m_path, ec);
            }
            if (ec)
            {
                DMK::Logger::get_instance().warning("[wardrobe-discovery] failed to commit registry '{}': {}",
                                                    m_path.string(), ec.message());
                return;
            }

            m_dirty = false;
        }

        void insert_name_locked(std::string_view internalName)
        {
            if (internalName.empty())
                return;
            if (m_names.insert(lower(internalName)).second)
                m_dirty = true;
        }

        void ingest_live_mappings_locked()
        {
            const auto &table = ItemNameTable::instance();
            if (!table.ready())
                return;

            // Anything already present in the live wardrobe state has, by definition, been interacted with in this
            // playthrough. Persist it so a stock-LT baseline selection cannot disappear from the spoiler-safe UI.
            for (const auto &m : slot_mappings())
            {
                if (!m.active || m.targetItemId == 0)
                    continue;
                insert_name_locked(table.name_of(m.targetItemId));
            }

            if (m_ready && m_dirty)
                save_locked();
        }

        void seed_canonical_locked()
        {
            // Canonical acquired/encountered equipment from FREQ-EE/ludomancy CURRENT_STATE.md plus confirmed
            // pre-snapshot play history recovered from the user's own project conversations as of 2026-09-07.
            // These are DISPLAY names only. Resolve at most ONE ordinary male/generic non-variant row per display name;
            // this prevents a duplicate display label from accidentally seeding hidden NPC/body variants.
            static constexpr std::array<std::string_view, 49> kSeedDisplayNames = {
                "Finely Crafted Gold Necklace",
                "Worn Ring",
                "Tarnished Ring",
                "Axiom Bracelet",
                "Engraved Gold Earring",
                "Replenishing Arrows",
                "Olvald's Logging Axe",
                "Lantern",
                "Bolton Long Sword",
                "Varnian Dagger",
                "Tauria Curved Sword",
                "Sword of the Lord",
                "Gray Wolf Bow",
                "Grey Wolf Bow",
                "Delizian Musket",
                "Staglord's Shield",
                "Mirror of Night",
                "Kite Shield",
                "Ator's Will Helm",
                "Doventry Leather Armour",
                "Shadowleaf Gloves",
                "Odeck's Protector Plate Boots",
                "Duskfang Leather Gloves",
                "Blackwing Mask",
                "Blackwing Leather Armour",
                "Skyblazer Cloth Helm",
                "Hanandian Leather Boots",
                "Hernandian Leather Boots",
                "Camouflage Outfit",
                "Finely Crafted Gold Ring",
                "Oath of Darkness",
                "Criminal Mask",
                "Criminal-behaviour Mask",
                "Criminal Behaviour Mask",
                // Confirmed historical gear from 2026-08-28, before the wardrobe registry existed.
                "Enithium Leather Cloak",
                "Enithium Leather Armour",
                "Enithium Leather Armor",
                "Enithium Leather Gloves",
                "Enithium Leather Boots",
                "Gray Wolf Wooden Shield",
                "Grey Wolf Wooden Shield",
                "Gray Wolf Sword",
                "Grey Wolf Sword",
                "Becker Axe",
                "Becker Dagger",
                "Becker Shield",
                "Sword of the Wolf",
                "Warspike Spear",
                "Herbalist's Pack",
            };

            std::unordered_set<std::string> wanted;
            std::unordered_set<std::string> resolvedDisplay;
            wanted.reserve(kSeedDisplayNames.size());
            resolvedDisplay.reserve(kSeedDisplayNames.size());
            for (const auto s : kSeedDisplayNames)
                wanted.insert(lower(s));

            std::size_t resolved = 0;
            using BK = ItemNameTable::BodyKind;
            for (const auto &e : ItemNameTable::instance().sorted_entries())
            {
                if (e.displayName.empty() || e.hasVariantMeta || e.category == TransmogSlot::Count)
                    continue;
                if (!(e.bodyKind == BK::Generic || e.bodyKind == BK::Male || e.bodyKind == BK::Both))
                    continue;

                const auto displayKey = lower(e.displayName);
                if (!wanted.contains(displayKey) || resolvedDisplay.contains(displayKey))
                    continue;

                insert_name_locked(e.name);
                resolvedDisplay.insert(displayKey);
                ++resolved;
            }

            DMK::Logger::get_instance().info("[wardrobe-discovery] canonical seed resolved {} unique catalog row(s)",
                                             resolved);
        }

        mutable std::mutex m_mutex;
        std::unordered_set<std::string> m_names;
        std::filesystem::path m_path;
        bool m_ready = false;
        bool m_dirty = false;
    };

} // namespace Transmog::Wardrobe
