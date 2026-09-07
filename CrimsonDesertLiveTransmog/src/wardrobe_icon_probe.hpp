#pragma once

#include "prefab_wrapper_swap.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace Transmog::Wardrobe
{
    /**
     * Read-only probe for the game's AppearanceTableLoader name registry.
     *
     * The current ItemNameTable exposes item identity/slot/body metadata but no texture handle or icon key. The game
     * nevertheless contains knowledge/item-icon-looking asset names in its appearance registries. This probe records
     * those names once so the next runtime log can tell us whether a practical item->icon naming path exists before we
     * invest in D3D texture loading. It never changes game state and never reveals names in the UI.
     */
    class IconProbe
    {
    public:
        static void tick_once_world_ready()
        {
            if (s_done)
                return;

            std::size_t walked = 0;
            std::size_t matched = 0;
            PrefabWrapperSwap::for_each_loader_prefab_name([&](std::string_view sv)
            {
                ++walked;
                std::string lower(sv);
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool interesting = lower.find("itemicon") != std::string::npos ||
                                         lower.find("knowledgeimage") != std::string::npos ||
                                         lower.find("item_icon") != std::string::npos ||
                                         lower.find("equipicon") != std::string::npos;
                if (!interesting)
                    return;

                ++matched;
                if (matched <= 80)
                    DMK::Logger::get_instance().info("[wardrobe-icon-probe] asset-name {}: {}", matched,
                                                     std::string(sv));
            });

            // A zero walk means the loader singleton is not ready yet; retry on a later wardrobe frame.
            if (walked == 0)
                return;

            s_done = true;
            DMK::Logger::get_instance().info(
                "[wardrobe-icon-probe] complete: walked {} AppearanceTableLoader names, found {} icon-like name(s). "
                "ItemNameTable currently exposes no icon texture handle; this probe tests naming feasibility only.",
                walked, matched);
        }

    private:
        inline static bool s_done = false;
    };
} // namespace Transmog::Wardrobe
