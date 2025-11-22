#include "ActivityLifecycle.hpp"
#include <fmt/core.h>

using namespace nixb::coro_prototype;

int
main ()
{
  fmt::print ("=== Activity Lifecycle Demonstration ===\n\n");

  fmt::print ("This shows how coroutines can express activity lifecycles\n");
  fmt::print ("as linear, readable code instead of state machines.\n\n");

  // Example 1: Download activity
  fmt::print ("--- Download Activity Example ---\n");
  auto download = DownloadActivity (1, "https://cache.nixos.org/foo.nar");

  // In a real implementation with a scheduler, you'd do:
  // coro::sync_wait(download.run());
  //
  // For this prototype, just show the structure
  fmt::print ("Download lifecycle:\n");
  fmt::print ("  1. fade_in()           - animate opacity 0→1\n");
  fmt::print ("  2. download_with_progress() - show download progress\n");
  fmt::print ("  3. linger()            - stay visible for 2 seconds\n");
  fmt::print ("  4. fade_out()          - animate opacity 1→0\n");
  fmt::print ("  5. coroutine completes - can be cleaned up\n\n");

  // Example 2: Build activity
  fmt::print ("--- Build Activity Example ---\n");
  auto build = BuildActivity (2, "hello-2.12.drv");

  fmt::print ("Build lifecycle:\n");
  fmt::print ("  1. phase(\"unpack\")     - unpack sources\n");
  fmt::print ("  2. phase(\"patch\")      - apply patches\n");
  fmt::print ("  3. phase(\"configure\")  - ./configure\n");
  fmt::print ("  4. phase(\"build\")      - make\n");
  fmt::print ("  5. phase(\"install\")    - make install\n");
  fmt::print ("  6. linger(3s)          - show \"finished\" state\n");
  fmt::print ("  7. coroutine completes - cleanup\n\n");

  fmt::print ("=== Key Insight ===\n\n");
  fmt::print ("Compare this to your current approach:\n\n");

  fmt::print ("BEFORE (state machine in maps):\n");
  fmt::print ("  - ActivityInfo stored in unordered_map\n");
  fmt::print ("  - Flags: is_finished, has_progress, etc.\n");
  fmt::print ("  - Manual phase tracking: current_phase string\n");
  fmt::print ("  - Cleanup logic scattered across multiple places\n");
  fmt::print ("  - \"Linger\" requires checking timestamps manually\n\n");

  fmt::print ("AFTER (coroutine lifecycle):\n");
  fmt::print ("  - Each activity IS a coroutine\n");
  fmt::print ("  - Lifecycle is LINEAR CODE you can read top-to-bottom\n");
  fmt::print ("  - State lives in coroutine frame (automatic)\n");
  fmt::print ("  - Cleanup happens when coroutine completes (RAII)\n");
  fmt::print ("  - \"Linger\" is just: co_await sleep(2s);\n\n");

  fmt::print ("=== Next Steps ===\n\n");
  fmt::print ("To make this real:\n");
  fmt::print ("1. Integrate libcoro's thread_pool or io_scheduler\n");
  fmt::print ("2. Implement actual sleep() with timers\n");
  fmt::print ("3. Hook Nix events → coroutine resumption\n");
  fmt::print ("4. Render loop as coroutine that yields every frame\n");
  fmt::print ("5. Channel per activity for event delivery\n\n");

  return 0;
}
