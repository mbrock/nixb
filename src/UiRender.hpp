#pragma once

#include "UiTypes.hpp"
#include <string>

namespace nixb
{

// Helpers for rendering activity lines; kept separate from UI backend
// so rendering logic is isolated from terminal control.
std::string render_activity_line (const UiActivityLine &line, int cols);

} // namespace nixb
