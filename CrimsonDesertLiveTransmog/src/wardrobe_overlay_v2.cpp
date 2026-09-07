// Active Wardrobe overlay translation unit.
//
// Keep this stable filename in CMake so the validated Phase Two build recipe never needs to change merely because the
// UI implementation iterates. The current implementation lives in wardrobe_overlay_v3.cpp; it is included here and
// is NOT compiled separately.
//
// The standalone renderer currently bakes ImGui's built-in face at a DPI-aware size. Until the renderer-level system
// serif atlas lands, scale that face here so 5120x1440 no longer reads like a tiny developer overlay. V3's explicit
// frame/panel geometry is designed around this 1.25x glyph scale.
#define draw_overlay wardrobe_draw_overlay_impl
#include "wardrobe_overlay_v3.cpp"
#undef draw_overlay

namespace Transmog
{
    void draw_overlay()
    {
        auto &io = ImGui::GetIO();
        io.FontGlobalScale = 1.25f;
        wardrobe_draw_overlay_impl();
    }
} // namespace Transmog
