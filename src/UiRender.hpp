#pragma once

#include "UiTypes.hpp"
#include <string>

namespace nixb
{

// Format bytes for human-readable display
std::string format_bytes (int64_t bytes);

// Helpers for rendering activity lines; kept separate from UI backend
// so rendering logic is isolated from terminal control.
std::string render_activity_line (const UiActivityLine &line, int cols);

} // namespace nixb
