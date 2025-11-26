#include "app.hpp"
#include "tty/units.hpp"
#include "tui.hpp"

#include <coro/coro.hpp>
#include <fmt/core.h>
#include <vector>

namespace nxb::tui_example
{

using namespace nxb::tui;

struct Activity
{
  std::string label;
  percent_t progress{ 0.0 * percent };
};

int
example (int argc, char *argv[])
{
  using namespace std::chrono_literals;
  using namespace nxb::tui_example;
  using nxb::percent;

  int steps = 10;
  if (argc > 1 && std::string_view (argv[1]) == "--steps")
    steps = std::stoi (argv[2]);

  return nxb::ui::run (
      std::vector<Activity> ({
          { "nixpkgs.tar.gz" },
          { "rustc.tar.xz" },
          { "llvm-17.src.tar.xz" },
      }),
      [] (auto &state) {
        return column (
            styled_text (span ("Build ", fg (Rgba8::magenta ()) | bold),
                         span ("Progress", fg (Rgba8::blue ()) | italic)),
            hrule (),
            list (
                state,
                [] (const auto &act) {
                  return row (
                      text (fmt::format ("{:<20}", act.label),
                            act.progress >= 100 * percent
                                ? (fg (Rgba8::green ()) | bold)
                                : fg (Rgba8::blue ())),
                      progress_bar (act.progress),
                      text (
                          fmt::format (
                              "{:>4.0f}%",
                              act.progress.force_numerical_value_in (percent)),
                          (act.progress >= 1 * one     ? fg (Rgba8::green ())
                           : act.progress >= 0.5 * one ? fg (Rgba8::yellow ())
                                                       : fg (Rgba8::white ()))
                              | bold));
                }),
            hrule ());
      },
      [n = steps] (nxb::ui::UIRuntime &runtime,

                   std::vector<Activity> &state) -> coro::task<> {
        for (int frame = 0; frame <= n; ++frame)
          {
            if (runtime.shutdown_requested ())
              co_return;

            auto x = frame * percent / (n / 100.0);

            state[0].progress = std::min (x / 0.8, 100.0 * percent);
            state[1].progress = std::min (x / 0.6, 100.0 * percent);
            state[2].progress = std::min (x / 0.4, 100.0 * percent);

            co_await runtime.scheduler ().yield_for (16ms);
          }

        co_await runtime.scheduler ().yield_for (1s);
      });
}

} // namespace nxb::tui_example

int
main (int argc, char *argv[])
{
  return nxb::tui_example::example (argc, argv);
}
