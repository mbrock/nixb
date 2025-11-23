#pragma once

#include <coro/coro.hpp>
#include "coro/queue.hpp"
#include <coro/io_scheduler.hpp>
#include <string>

#include <nix/util/logging.hh>

namespace nixb::coro_adapter
{

struct log_message
{
  nix::Verbosity level;
  std::string text;
};

struct activity_started
{
  int64_t id;
  nix::ActivityType type;
  nix::Verbosity level;
  std::string text;
  int64_t parent;
  std::vector<std::variant<int64_t, std::string>> fields;
};

struct activity_progress
{
  int64_t id;
  nix::ResultType type;
  std::vector<std::variant<int64_t, std::string>> fields;
};

struct activity_stopped
{
  int64_t id;
};

using log_event = std::variant<log_message, nix::ErrorInfo, activity_started,
                               activity_progress, activity_stopped>;

class nix_log_adapter : public nix::Logger
{
public:
  explicit nix_log_adapter (coro::queue<log_event> &queue);

  void log (nix::Verbosity lvl, std::string_view s) override;
  void logEI (const nix::ErrorInfo &ei) override;
  void startActivity (nix::ActivityId act, nix::Verbosity lvl,
                      nix::ActivityType type, const std::string &text,
                      const Fields &fields, nix::ActivityId parent) override;
  void stopActivity (nix::ActivityId act) override;
  void result (nix::ActivityId act, nix::ResultType type,
               const Fields &fields) override;

private:
  static std::vector<std::variant<int64_t, std::string>>
  extract_fields (const Fields &fields);

  coro::queue<log_event> &queue_;
};

} // namespace nixb::coro_adapter
