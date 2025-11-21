#pragma once

#include "TerminalUi.hpp"
#include <string>

namespace nixb
{

// Helpers for rendering activity lines; kept separate from TerminalUi so the
// UI only concerns itself with layout/scroll behavior.
std::string render_activity_line (const UiActivityLine &line, int cols);

} // namespace nixb
