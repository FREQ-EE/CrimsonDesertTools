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
     * Version 1 accidentally re-ingested transient slot_mappings() every UI frame. Hover previews and unrestricted
     * catalogue browsing could therefore pollute the file with appearances the player had never acquired. Version 2
     * deliberately starts from a new filename and NEVER learns from preview/transmog state. Only canonical historical
     * seed data and explicit future acquisition hooks may call mark_discovered()/mark_id().
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
                return;
            if (!ItemNameTable::instance().ready())
                return;

            m_path = runtime_path();
            load_locked();
            seed_canonical_locked();
            m_ready = true;
            if (m_dirty)
                save_locked();

            DMK::Logger::get_instance().info(
                "[wardrobe-discovery] v2 ready: {} stable appearance name(s), file='{}' (legacy v1 ignored)",
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

        // Reserved for trustworthy acquisition/inventory hooks. UI hover/picks MUST NOT call this.
        void mark_discovered(std::string_view internalName, const char *reason = "acquisition")
        {
            if (internalName.empty())
                return;
            std::scoped_lock lock(m_mutex);
            const auto key = lower(internalName);
            if (m_names.insert(key).second)
            {
                m_dirty = true;
                DMK::Logger::get_instance().info("[wardrobe-discovery] learned '{}' ({})", internalName,
                                                 reason ? reason : "acquisition");
                if (m_ready)
                    save_locked();
            }
        }

        void mark_id(std::uint16_t id, const char *reason = "acquisition")
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
            wchar_t buf[32768]{};
            const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            if (n == 0 || n >= std::size(buf))
                return std::filesystem::path("CrimsonDesertLiveTransmog_discovered_v2.json");
            auto p = std::filesystem::path(std::wstring_view(buf, n));
            return p.parent_path() / L"CrimsonDesertLiveTransmog_discovered_v2.json";
        }

        void load_locked()
        {
            m_names.clear();
            m_dirty = false;
            std::ifstream f(m_path);
            if (!f.is_open())
            {
                DMK::Logger::get_instance().info(
                    "[wardrobe-discovery] no v2 registry yet; starting from verified play-history seed");
                return;
            }

            try
            {
                const auto root = nlohmann::json::parse(f);
                if (!root.is_object() || root.value("version", 0) != 2 || !root.contains("discovered") ||
                    !root["discovered"].is_array())
                {
                    DMK::Logger::get_instance().warning(
                        "[wardrobe-discovery] v2 schema invalid; failing closed to verified seed only");
                    return;
                }
                for (const auto &v : root["discovered"])
                    if (v.is_string())
                    {
                        const auto name = lower(v.get<std::string>());
                        if (!name.empty())
                            m_names.insert(name);
                    }
            }
            catch (const std::exception &e)
            {
                DMK::Logger::get_instance().warning(
                    "[wardrobe-discovery] failed to parse v2 registry ({}); failing closed to verified seed only", e.what());
                m_names.clear();
            }
        }

        void save_locked()
        {
            if (m_path.empty())
                return;
            nlohmann::json root;
            root["version"] = 2;
            root["identity"] = "stable internal item names; acquisition-only learning";
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
                std::filesystem::remove(m_path, ec);
                ec.clear();
                std::filesystem::rename(tmp, m_path, ec);
            }
            if (ec)
            {
                DMK::Logger::get_instance().warning("[wardrobe-discovery] failed to commit v2 registry '{}': {}",
                                                    m_path.string(), ec.message());
                return;
            }
            m_dirty = false;
        }

        void insert_name_locked(std::string_view internalName)
        {
            if (!internalName.empty() && m_names.insert(lower(internalName)).second)
                m_dirty = true;
        }

        void seed_canonical_locked()
        {
            // Display names that are documented as genuinely acquired/encountered in FREQEE's playthrough. Aliases are
            // intentional because the display-name TSV and live UI have used a few spelling variants across patches.
            static constexpr std::array<std::string_view, 49> kSeedDisplayNames = {
                "Finely Crafted Gold Necklace", "Worn Ring", "Tarnished Ring", "Axiom Bracelet",
                "Engraved Gold Earring", "Replenishing Arrows", "Olvald's Logging Axe", "Lantern",
                "Bolton Long Sword", "Varnian Dagger", "Tauria Curved Sword", "Sword of the Lord",
                "Gray Wolf Bow", "Grey Wolf Bow", "Delizian Musket", "Staglord's Shield", "Mirror of Night",
                "Kite Shield", "Ator's Will Helm", "Doventry Leather Armour", "Shadowleaf Gloves",
                "Odeck's Protector Plate Boots", "Duskfang Leather Gloves", "Blackwing Mask",
                "Blackwing Leather Armour", "Skyblazer Cloth Helm", "Hanandian Leather Boots",
                "Hernandian Leather Boots", "Camouflage Outfit", "Finely Crafted Gold Ring", "Oath of Darkness",
                "Criminal Mask", "Criminal-behaviour Mask", "Criminal Behaviour Mask",
                "Enithium Leather Cloak", "Enithium Leather Armour", "Enithium Leather Armor",
                "Enithium Leather Gloves", "Enithium Leather Boots", "Gray Wolf Wooden Shield",
                "Grey Wolf Wooden Shield", "Gray Wolf Sword", "Grey Wolf Sword", "Becker Axe", "Becker Dagger",
                "Becker Shield", "Sword of the Wolf", "Warspike Spear", "Herbalist's Pack",
            };

            std::unordered_set<std::string> wanted;
            std::unordered_set<std::string> resolvedDisplay;
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
            DMK::Logger::get_instance().info(
                "[wardrobe-discovery] v2 verified seed resolved {} unique catalog row(s)", resolved);
        }

        mutable std::mutex m_mutex;
        std::unordered_set<std::string> m_names;
        std::filesystem::path m_path;
        bool m_ready = false;
        bool m_dirty = false;
    };
} // namespace Transmog::Wardrobe
