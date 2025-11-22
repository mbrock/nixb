#include "CoroPrototype.hpp"

#include <fmt/core.h>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/logging.hh>

using namespace nixb::coro_prototype;

int
main ()
{
  fmt::print ("=== libcoro + Nix Logger Prototype ===\n\n");

  // Initialize Nix
  nix::initLibStore ();

  // Create the coroutine dispatcher
  auto dispatcher = std::make_shared<CoroEventDispatcher> ();

  // Install our coroutine-based logger
  auto coro_logger = std::make_unique<CoroNixLogger> (dispatcher);
  nix::logger = std::move (coro_logger);

  // Open a Nix store (this will trigger some activities)
  fmt::print ("Opening Nix store...\n");
  auto store = nix::openStore ();

  fmt::print ("\nStore opened. Logger is now active.\n");
  fmt::print ("Try running: nix-build '<nixpkgs>' -A hello\n");
  fmt::print ("Or any Nix command that generates build activities.\n\n");

  // In a real version, you'd:
  // 1. Run a build: store->buildPaths(...)
  // 2. Have a scheduler that runs the coroutines
  // 3. Have proper async I/O integration

  // For now, just demonstrate the structure
  fmt::print ("Simulating some activity events...\n\n");

  // Manually trigger some events to show the pattern
  dispatcher->on_activity_start (
      ActivityStarted{ .id = 1,
                       .type = nix::actBuild,
                       .text = "building 'hello-2.12'",
                       .parent = 0 });

  dispatcher->on_activity_progress (ActivityProgress{
      .id = 1, .type = nix::resProgress, .data = "configuring..." });

  dispatcher->on_activity_progress (
      ActivityProgress{ .id = 1, .type = nix::resProgress, .data = "building..." });

  dispatcher->on_activity_stop (ActivityStopped{ .id = 1 });

  fmt::print ("\n=== Prototype complete ===\n");
  fmt::print ("Next steps:\n");
  fmt::print ("- Integrate proper libcoro scheduler\n");
  fmt::print ("- Add real timing/delays for linger phase\n");
  fmt::print ("- Connect to actual Nix build operations\n");
  fmt::print ("- Add UI rendering coroutine\n");

  return 0;
}
