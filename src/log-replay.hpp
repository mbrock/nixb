#pragma once

#include <nxt/units.hpp>

#include "nix-log-adapter.hpp"
#include "nxt/app.hpp"

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/queue.hpp>
#include <istream>
#include <string>

namespace nixb::replay {

using Event = nix_event::NixLogEvent;

/// Replay log events from a .tnixlog or .json file to an event
/// queue. Parses JSON and converts to semantic events, pushing
/// async to the queue.
///
/// If realtime is true, delays between events match the original
/// timestamps. If speed > 1.0, playback is faster; if < 1.0,
/// slower. Pass a stop_token to enable cancellation.
coro::task<> replay_file(nxb::ui::UIRuntime& runtime,
    int fd,
    std::istream& input,
    coro::queue<Event>& queue,
    bool realtime = true,
    double speed = 1.0);

/// Replay from a file path (opens/closes the file).
coro::task<> replay_file(nxb::ui::UIRuntime& runtime,
    const std::string& path,
    coro::queue<Event>& queue,
    bool realtime = true,
    double speed = 1.0);

} // namespace nixb::replay
