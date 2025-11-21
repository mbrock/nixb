#include <CLI/CLI.hpp>

#include "NixLogWatcher.hpp"

int main(int argc, char **argv) {
  CLI::App app{"nixb - minimal nix internal-json watcher"};
  bool quiet = false;
  app.add_flag("-q,--quiet", quiet,
               "suppress pass-through lines that are not @nix JSON");
  CLI11_PARSE(app, argc, argv);

  nixb::NixLogWatcher watcher(quiet);
  watcher.process_input();

  return 0;
}
