#include "nix-log-adapter.hpp"

namespace nixb::nix_event {

std::string get_string_field(const Fields & fields, const size_t idx)
{
    if (idx >= fields.size())
        return {};
    if (fields[idx].type == nix::Logger::Field::tString)
        return fields[idx].s;
    return {};
}

int64_t get_int_field(const Fields & fields, const size_t idx)
{
    if (idx >= fields.size())
        return 0;
    if (fields[idx].type == nix::Logger::Field::tInt)
        return static_cast<int64_t>(fields[idx].i);
    return 0;
}

activity::Kind parse_activity_kind(
    const nix::ActivityType type,
    const std::string & text,
    const Fields & fields)
{
    using namespace activity;

    switch (type) {
    case nix::actBuild:
        return Build{
            .drv_path = get_string_field(fields, 0),
            .output = get_string_field(fields, 1),
        };

    case nix::actFileTransfer:
        return Download{
            .url = get_string_field(fields, 0),
        };

    case nix::actCopyPath:
        return Copy{
            .path = get_string_field(fields, 0),
            .from_store = get_string_field(fields, 1),
            .to_store = get_string_field(fields, 2),
        };

    case nix::actRealise:
        return Realise{
            .path = get_string_field(fields, 0),
        };

    case nix::actSubstitute:
        return Substitute{
            .path = get_string_field(fields, 0),
            .from_store = get_string_field(fields, 1),
        };

    case nix::actQueryPathInfo:
        return QueryPathInfo{
            .path = get_string_field(fields, 0),
            .from_store = get_string_field(fields, 1),
        };

    case nix::actPostBuildHook:
        return PostBuildHook{
            .drv_path = get_string_field(fields, 0),
        };

    case nix::actBuildWaiting:
        return BuildWaiting{};

    default:
        return Unknown{
            .type = type,
            .text = text,
        };
    }
}

std::optional<NixLogEvent> parse_result(
    const ActivityId id, const nix::ResultType type, const Fields & fields)
{
    switch (type) {
    case nix::resProgress:
        return ActivityProgress{
            .id = id,
            .done = get_int_field(fields, 0),
            .expected = get_int_field(fields, 1),
            .running = get_int_field(fields, 2),
            .failed = get_int_field(fields, 3),
        };

    case nix::resSetPhase:
        return ActivityPhase{
            .id = id,
            .phase = get_string_field(fields, 0),
        };

    case nix::resBuildLogLine:
    case nix::resPostBuildLogLine:
        return LogLine{
            .level = nix::lvlInfo,
            .text = get_string_field(fields, 0),
        };

    default:
        return std::nullopt;
    }
}

} // namespace nixb::nix_event

namespace nixb::coro_adapter {

namespace ev = nix_event;

NixLogAdapter::NixLogAdapter(coro::queue<Event> & queue)
    : queue_(queue)
{
}

void NixLogAdapter::log(const nix::Verbosity lvl, const std::string_view s)
{
    coro::sync_wait(
        queue_.push(ev::LogLine{.level = lvl, .text = std::string(s)}));
}

void NixLogAdapter::logEI(const nix::ErrorInfo & ei)
{
    coro::sync_wait(queue_.push(ev::Error{.info = ei}));
}

void NixLogAdapter::startActivity(
    const nix::ActivityId act,
    const nix::Verbosity /*lvl*/,
    const nix::ActivityType type,
    const std::string & text,
    const Fields & fields,
    const nix::ActivityId parent)
{
    auto kind = ev::parse_activity_kind(type, text, fields);
    coro::sync_wait(queue_.push(
        ev::ActivityStarted{
            .id = ev::ActivityId{static_cast<int64_t>(act)},
            .parent = ev::ActivityId{static_cast<int64_t>(parent)},
            .kind = std::move(kind),
        }));
}

void NixLogAdapter::stopActivity(const nix::ActivityId act)
{
    coro::sync_wait(queue_.push(
        ev::ActivityFinished{
            .id = ev::ActivityId{static_cast<int64_t>(act)}}));
}

void NixLogAdapter::result(
    const nix::ActivityId act,
    const nix::ResultType type,
    const Fields & fields)
{
    auto id = ev::ActivityId{static_cast<int64_t>(act)};
    if (auto event = ev::parse_result(id, type, fields))
        coro::sync_wait(queue_.push(std::move(*event)));
}

} // namespace nixb::coro_adapter
