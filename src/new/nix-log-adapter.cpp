#include "nix-log-adapter.hpp"

#include <fmt/core.h>
#include <nix/util/error.hh>

namespace nixb::coro_adapter
{

nix_log_adapter::nix_log_adapter (coro::queue<log_event> &queue)
    : queue_ (queue)
{
}

void
nix_log_adapter::log (nix::Verbosity lvl, std::string_view s)
{
  coro::sync_wait (
      queue_.push (log_message{ .level = lvl, .text = std::string (s) }));
}

void
nix_log_adapter::logEI (const nix::ErrorInfo &ei)
{
  coro::sync_wait (queue_.push (std::move (ei)));
}

void
nix_log_adapter::startActivity (nix::ActivityId act, nix::Verbosity lvl,
                                nix::ActivityType type,
                                const std::string &text,
                                const nix::Logger::Fields &fields,
                                nix::ActivityId parent)
{
  coro::sync_wait (
      queue_.push (activity_started{ .id = static_cast<int64_t> (act),
                                     .type = type,
                                     .level = lvl,
                                     .text = text,
                                     .parent = static_cast<int64_t> (parent),
                                     .fields = extract_fields (fields) }));
}

void
nix_log_adapter::stopActivity (nix::ActivityId act)
{
  coro::sync_wait (
      queue_.push (activity_stopped{ .id = static_cast<int64_t> (act) }));
}

void
nix_log_adapter::result (nix::ActivityId act, nix::ResultType type,
                         const nix::Logger::Fields &fields)
{
  coro::sync_wait (
      queue_.push (activity_progress{ .id = static_cast<int64_t> (act),
                                      .type = type,
                                      .fields = extract_fields (fields) }));
}

std::vector<std::variant<int64_t, std::string>>
nix_log_adapter::extract_fields (const nix::Logger::Fields &fields)
{
  std::vector<std::variant<int64_t, std::string>> result;
  result.reserve (fields.size ());

  for (const auto &field : fields)
    {
      if (field.type == nix::Logger::Field::tString)
        {
          result.push_back (field.s);
        }
      else
        {
          result.push_back (static_cast<int64_t> (field.i));
        }
    }

  return result;
}

} // namespace nixb::coro_adapter
