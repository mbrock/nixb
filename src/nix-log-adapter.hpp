#pragma once

// Undefine the 'unix' macro from libc to avoid conflict with
// nix::unix namespace
#ifdef unix
#  undef unix
#endif

#include <string>
#include <variant>

#include <nix/util/logging.hh>

#include <coro/coro.hpp>
#include <coro/queue.hpp>

namespace nixb::nix_event {

/// Strong type for activity IDs to prevent mixing with other
/// int64_t values.
struct ActivityId
{
    int64_t value;

    explicit constexpr ActivityId(int64_t v)
        : value(v)
    {
    }

    constexpr bool operator==(const ActivityId &) const = default;
};

// Activity kinds - what type of activity started
namespace activity {

struct Build
{
    std::string drv_path;
    std::string output;
};

struct Download
{
    std::string url;
};

struct Copy
{
    std::string path;
    std::string from_store;
    std::string to_store;
};

struct Realise
{
    std::string path;
};

struct Substitute
{
    std::string path;
    std::string from_store;
};

struct QueryPathInfo
{
    std::string path;
    std::string from_store;
};

struct PostBuildHook
{
    std::string drv_path;
};

struct BuildWaiting
{};

struct Unknown
{
    nix::ActivityType type;
    std::string text;
};

using Kind =
    std::variant<Build, Download, Copy, Realise, Substitute, QueryPathInfo, PostBuildHook, BuildWaiting, Unknown>;

} // namespace activity

// Activity lifecycle events
struct ActivityStarted
{
    ActivityId id;
    ActivityId parent;
    activity::Kind kind;
};

struct ActivityProgress
{
    ActivityId id;
    int64_t done;
    int64_t expected;
    int64_t running;
    int64_t failed;
};

struct ActivityPhase
{
    ActivityId id;
    std::string phase;
};

struct ActivityFinished
{
    ActivityId id;
};

// Non-activity events
struct LogLine
{
    nix::Verbosity level;
    std::string text;
};

struct Error
{
    nix::ErrorInfo info;
};

using NixLogEvent = std::variant<ActivityStarted, ActivityProgress, ActivityPhase, ActivityFinished, LogLine, Error>;

// Parsing helpers - convert nix logger types to semantic events
using Fields = nix::Logger::Fields;

std::string get_string_field(const Fields & fields, size_t idx);
int64_t get_int_field(const Fields & fields, size_t idx);

activity::Kind parse_activity_kind(nix::ActivityType type, const std::string & text, const Fields & fields);

std::optional<NixLogEvent> parse_result(ActivityId act, nix::ResultType type, const Fields & fields);

} // namespace nixb::nix_event

namespace nixb::coro_adapter {

using Event = nix_event::NixLogEvent;

/// Logger adapter that pushes semantic events to a queue.
/// Note: Uses sync_wait internally, so not suitable for async
/// contexts. For async replay, use nixb::replay functions
/// instead.
class NixLogAdapter : public nix::Logger
{
public:
    explicit NixLogAdapter(coro::queue<Event> & queue);

    void log(nix::Verbosity lvl, std::string_view s) override;
    void logEI(const nix::ErrorInfo & ei) override;
    void startActivity(
        nix::ActivityId act,
        nix::Verbosity lvl,
        nix::ActivityType type,
        const std::string & text,
        const Fields & fields,
        nix::ActivityId parent) override;
    void stopActivity(nix::ActivityId act) override;
    void result(nix::ActivityId act, nix::ResultType type, const Fields & fields) override;

private:
    coro::queue<Event> & queue_;
};

} // namespace nixb::coro_adapter
