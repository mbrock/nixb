#include "nxt/signal-pipe.hpp"
#include "uring.hpp"

#include <csignal>
#include <exec/repeat_effect_until.hpp>
#include <exec/when_any.hpp>
#include <fmt/core.h>

namespace sx = stdexec;

namespace nxb::stdexec_demo
{

int
example ()
{
  using namespace std::chrono_literals;

  uring::io_uring_runtime runtime;
  auto sched = runtime.scheduler ();

  ui::SignalPipe signals;
  signals.watch (SIGINT, SIGTERM, SIGWINCH);

  uring::async_event damage_event;
  std::atomic<int> counter{ 0 };

  // Signal loop: keep polling until shutdown signal
  auto signal_loop
      = uring::async_poll (sched, signals.read_fd ())
        | sx::then (
            [&] (short) -> bool
            {
              while (auto sig = signals.try_read ())
                {
                  switch (*sig)
                    {
                    case SIGWINCH:
                      fmt::print ("SIGWINCH\n");
                      damage_event.set ();
                      break;
                    case SIGINT:
                    case SIGTERM:
                      fmt::print ("Shutdown signal\n");
                      return true; // stop repeating
                    }
                }
              return false;
            })
        | exec::repeat_effect_until ();

  // Render loop: infinite until cancelled
  auto render_loop
      = damage_event.wait (sched)
        | sx::then (
            [&]
            {
              fmt::print ("Render frame {}\n", counter.load ());
              return false; // never stop on our own
            })
        | exec::repeat_effect_until ();

  // Tick N times then complete
  auto tick_loop
      = exec::schedule_after (sched, 200ms)
        | sx::then (
            [&] () -> bool
            {
              auto c = counter.fetch_add (1) + 1;
              damage_event.set ();
              return c >= 10; // stop after 10 ticks
            })
        | exec::repeat_effect_until ();

  // when_any: first to complete cancels the others!
  // - signal_loop completes on Ctrl-C
  // - tick_loop completes after 10 ticks
  // - render_loop never completes on its own (infinite)
  auto app = exec::when_any (signal_loop, render_loop, tick_loop);

  sx::sync_wait (app);

  fmt::print ("Done! counter={}\n", counter.load ());
  return 0;
}

} // namespace nxb::stdexec_demo

int
main ()
{
  return nxb::stdexec_demo::example ();
}
