#include <CLI/CLI.hpp>
#include <fmt/color.h>
#include <fmt/core.h>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "nix_log_parser.hpp"

using namespace nixb;

namespace {

struct ActivityInfo {
  ActivityType type;
  std::string text;
};

class NixLogWatcher {
public:
  explicit NixLogWatcher(bool quiet) : quiet_(quiet) {
    nix::initLibStore();
    store_ = nix::openStore();
    fmt::print(stderr, "Yay! Nix store opened successfully: {}\n",
               store_->config.getHumanReadableURI());
  }

  void process_input() {
    std::string line;
    while (std::getline(std::cin, line)) {
      process_line(line);
    }
  }

private:
  void process_line(const std::string &line) {
    auto event_opt = parser_.parse_line(line);

    if (!event_opt) {
      if (!quiet_) {
        fmt::print("{}\n", line);
      }
      return;
    }

    auto &event = event_opt.value();

    if (std::holds_alternative<StartEvent>(event)) {
      handle_start_event(std::get<StartEvent>(event));
    } else if (std::holds_alternative<ResultEvent>(event)) {
      handle_result_event(std::get<ResultEvent>(event));
    } else if (std::holds_alternative<StopEvent>(event)) {
      handle_stop_event(std::get<StopEvent>(event));
    } else if (std::holds_alternative<MsgEvent>(event)) {
      handle_msg_event(std::get<MsgEvent>(event));
    }
  }

  void handle_start_event(const StartEvent &e) {
    activities_[e.id] = ActivityInfo{e.type, e.text};
    if (e.type == nix::actBuilds) {
      builds_activity_ = e.id;
    }
    fmt::print("{}", e.format());
  }

  void handle_result_event(const ResultEvent &e) {
    fmt::print("{}", e.format());
    update_success_tokens(e);
  }

  void handle_stop_event(const StopEvent &e) {
    std::string_view type_name = "Unknown";
    std::string activity_text;
    bool build_success = false;

    if (auto it = activities_.find(e.id); it != activities_.end()) {
      type_name = NixLogParser::activity_type_name(it->second.type);
      if (it->second.type == nix::actBuild && success_tokens_ > 0) {
        build_success = true;
        --success_tokens_;
      }
      activity_text = it->second.text;
      activities_.erase(it);
    }

    fmt::print("{}", e.format(type_name, activity_text, build_success));
  }

  void handle_msg_event(const MsgEvent &e) { fmt::print("{}", e.format()); }

  void update_success_tokens(const ResultEvent &e) {
    if (e.type != nix::resProgress || !builds_activity_ ||
        *builds_activity_ != e.id) {
      return;
    }
    if (auto done = e.get_int(0)) {
      if (*done > last_progress_done_) {
        success_tokens_ += (*done - last_progress_done_);
        last_progress_done_ = *done;
      }
    }
  }

  bool quiet_;
  NixLogParser parser_;
  std::shared_ptr<nix::Store> store_;
  std::unordered_map<int64_t, ActivityInfo> activities_;
  std::optional<int64_t> builds_activity_;
  int64_t success_tokens_ = 0;
  int64_t last_progress_done_ = 0;
};

} // namespace

int main(int argc, char **argv) {
  CLI::App app{"nixb - minimal nix internal-json watcher"};
  bool quiet = false;
  app.add_flag("-q,--quiet", quiet,
               "suppress pass-through lines that are not @nix JSON");
  CLI11_PARSE(app, argc, argv);

  NixLogWatcher watcher(quiet);
  watcher.process_input();

  return 0;
}
