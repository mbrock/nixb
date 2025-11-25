#include "fmt/base.h"

#include "nix-log-adapter.hpp"
#include "nix/util/error.hh"

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>
#include <nix/expr/eval-gc.hh>
#include <nix/store/store-api.hh>
#include <nix/util/logging.hh>

using coro::io_scheduler;
using coro::latch;
using coro::queue;
using coro::sync_wait;
using coro::task;
using coro::when_all;
using fmt::println;
using std::get;
using std::ref;
using std::unique_ptr;

using namespace nixb::coro_adapter;
using namespace std::chrono_literals;

int
main ()
{
  println ("main: starting");
  nix::initLibUtil ();
  nix::initGC ();

  println ("main: creating scheduler");
  auto the_scheduler = io_scheduler::make_unique (io_scheduler::options{});

  println ("main: creating queue");
  auto log_queue = queue<log_event>{};
  latch producer_done{ 1 }; // Track when producer finishes

  println ("main: setting up nix logger");
  nix::logger = make_unique<nix_log_adapter> (ref (log_queue));

  auto log_consumer_task = [] (const unique_ptr<io_scheduler> &sched,
                               queue<log_event> &queue_) -> task<>
    {
      println ("Consumer: starting");
      co_await sched->schedule ();
      println ("Consumer: scheduled on io_scheduler");

      while (true)
        {
          auto expected = co_await queue_.pop ();
          if (!expected)
            {
              println ("Consumer: queue shutdown, exiting");
              break;
            }

          if (std::holds_alternative<log_message> (*expected))
            {
              auto &[level, text] = get<log_message> (*expected);
              println ("LOG {}: {}", static_cast<int> (level), text);
            }
          else if (std::holds_alternative<activity_started> (*expected))
            {
              auto &activity = get<activity_started> (*expected);
              println ("ACTIVITY START {}: {}", activity.id, activity.text);
            }
          else if (std::holds_alternative<activity_stopped> (*expected))
            {
              auto &[id] = get<activity_stopped> (*expected);
              println ("ACTIVITY STOP {}", id);
            }
          else if (std::holds_alternative<activity_progress> (*expected))
            {
              auto &activity = get<activity_progress> (*expected);
              println ("ACTIVITY PROGRESS {}", activity.id);
            }
          else if (std::holds_alternative<nix::ErrorInfo> (*expected))
            {
              auto &error = get<nix::ErrorInfo> (*expected);
              println ("ERROR: {}", nix::BaseError (error).what ());
            }
        }
      co_return;
    };

  auto log_producer_task
      = [] (const unique_ptr<io_scheduler> &sched, latch &done) -> task<>
    {
      println ("Producer: starting");
      co_await sched->schedule ();
      println ("Producer: scheduled on io_scheduler");

      nix::ActivityId build_act = 1000;
      nix::logger->startActivity (build_act, nix::lvlInfo, nix::actBuild,
                                  "Building coroutine demo", {}, 0);
      co_await sched->schedule_after (100ms);

      nix::ActivityId fetch_act = 1001;
      nix::logger->startActivity (fetch_act, nix::lvlInfo, nix::actFetchTree,
                                  "Fetching dependencies", {}, build_act);

      for (int i = 0; i < 3; ++i)
        {
          nix::logger->result (
              fetch_act, nix::resProgress,
              {
                  nix::Logger::Field (static_cast<uint64_t> (i + 1)),
                  nix::Logger::Field (static_cast<uint64_t> (3)),
                  nix::Logger::Field (static_cast<uint64_t> (1)),
                  nix::Logger::Field (static_cast<uint64_t> (0)),
              });
          co_await sched->schedule_after (150ms);
        }

      nix::logger->stopActivity (fetch_act);
      co_await sched->schedule_after (100ms);

      nix::logger->log (nix::lvlInfo, "Build step complete");
      nix::logger->stopActivity (build_act);
      nix::logger->log (nix::lvlNotice, "Producer coroutine finished!");

      println ("Producer: done, counting down latch");
      done.count_down ();
      co_return;
    };

  auto shutdown_task = [] (unique_ptr<io_scheduler> &sched,
                           queue<log_event> &queue_, latch &done) -> task<>
    {
      println ("Shutdown: starting");
      co_await sched->schedule ();
      println ("Shutdown: waiting for producer to finish");
      co_await done;
      println ("Shutdown: producer done, draining queue");
      co_await queue_.shutdown_drain (sched);
      println ("Shutdown: queue drained and shut down");
      co_return;
    };

  println ("main: launching tasks with when_all");
  sync_wait (
      when_all (log_consumer_task (the_scheduler, log_queue),
                log_producer_task (the_scheduler, producer_done),
                shutdown_task (the_scheduler, log_queue, producer_done)));

  return 0;
}
