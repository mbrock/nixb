#include "fmt/base.h"
#include "nix-log-adapter.hpp"
#include "nix/util/error.hh"

#include <nix/util/logging.hh>

using namespace nixb::coro_adapter;
using namespace std::chrono_literals;

int
main ()
{
  auto scheduler
      = coro::io_scheduler::make_unique (coro::io_scheduler::options{});

  auto queue = coro::queue<log_event>{};
  auto logger = std::make_unique<nix_log_adapter> (std::ref (queue));
  nix::logger = std::move (logger);

  auto task = [] (const std::unique_ptr<coro::io_scheduler> &sched,
                  coro::queue<log_event> &queue_) -> coro::task<> {
    co_await sched->schedule ();

    while (true)
      {
        auto expected = co_await queue_.pop ();
        if (!expected)
          break;

        if (std::holds_alternative<log_message> (*expected))
          {
            auto &[level, text] = std::get<log_message> (*expected);
            fmt::println ("{}: {}", static_cast<int> (level),
                          text);
          }
        else if (std::holds_alternative<activity_started> (*expected))
          {
            auto &activity = std::get<activity_started> (*expected);
            fmt::println ("start {}: {}", activity.id, activity.text);
          }
        else if (std::holds_alternative<activity_stopped> (*expected))
          {
            auto &[id] = std::get<activity_stopped> (*expected);
            fmt::println ("stop {}", id);
          }
        else if (std::holds_alternative<activity_progress> (*expected))
          {
            auto &activity = std::get<activity_progress> (*expected);
            fmt::println ("progress {}", activity.id);
          }
        else if (std::holds_alternative<nix::ErrorInfo> (*expected))
          {
            auto &error = std::get<nix::ErrorInfo> (*expected);
            fmt::println ("{}", nix::BaseError (error).what ());
          }
      }
    co_return;
  };

  coro::sync_wait (coro::when_all (task (scheduler, queue)));

  return 0;
}
