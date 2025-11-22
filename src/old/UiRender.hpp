#pragma once

#include "UiTypes.hpp"
#include <fmt/color.h>
#include <string>

namespace nixb
{

// Background color for HUD - matches the fade-to-oblivion endpoint
fmt::rgb get_hud_background_color ();

// Format bytes for human-readable display
std::string format_bytes (int64_t bytes);

// Render an activity line into the HUD raster at the specified row
void render_activity_line (HudRaster &raster, int row,
                           const UiActivityLine &line, int cols);

// Convert a HudRaster to ANSI output with uniform background color
// Returns a vector of strings, one per row
std::vector<std::string> raster_to_ansi (const HudRaster &raster,
                                         fmt::rgb bg_color);

} // namespace nixb
