#include <CLI/CLI.hpp>
#include <fmt/color.h>
#include <fmt/core.h>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>

#include <cstdio>
#include <iterator>
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

std::string_view trim_trailing_newline(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::optional<std::string_view> string_field(const ResultEvent &event,
                                             size_t idx) {
  if (idx >= event.fields.size() ||
      !std::holds_alternative<std::string>(event.fields[idx])) {
    return std::nullopt;
  }
  return std::get<std::string>(event.fields[idx]);
}

std::optional<int64_t> int_field(const ResultEvent &event, size_t idx) {
  if (idx >= event.fields.size() ||
      !std::holds_alternative<int64_t>(event.fields[idx])) {
    return std::nullopt;
  }
  return std::get<int64_t>(event.fields[idx]);
}

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
    if (e.type == ActivityType::Builds) {
      builds_activity_ = e.id;
    }

    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), "{} ",
                   fmt::styled("[start]", fmt::fg(fmt::terminal_color::blue)));
    fmt::format_to(std::back_inserter(buf), "{} ",
                   NixLogParser::activity_type_name(e.type));
    if (!e.text.empty()) {
      fmt::format_to(std::back_inserter(buf), "{}",
                     fmt::styled(e.text, fmt::fg(fmt::terminal_color::white)));
    }
    fmt::format_to(std::back_inserter(buf), "\n");
    fmt::print("{}", fmt::to_string(buf));
  }

  void handle_result_event(const ResultEvent &e) {
    bool printed_compact = false;

    auto print_log_line = [&](std::string_view msg_view) {
      auto faint_style =
          fmt::fg(fmt::terminal_color::white) | fmt::emphasis::faint;
      fmt::print("{}\n",
                 fmt::styled(fmt::format("> {}", msg_view), faint_style));
      printed_compact = true;
    };

    if (e.type == ResultType::BuildLogLine ||
        e.type == ResultType::PostBuildLogLine ||
        e.type == ResultType::FetchStatus) {
      if (auto msg = string_field(e, 0)) {
        print_log_line(trim_trailing_newline(*msg));
      }
    } else if (e.type == ResultType::SetPhase) {
      if (auto phase = string_field(e, 0)) {
        fmt::print(
            "{} {}\n",
            fmt::styled("[phase]", fmt::fg(fmt::terminal_color::magenta)),
            *phase);
        printed_compact = true;
      }
    }

    if (printed_compact) {
      update_success_tokens(e);
      return;
    }

    print_verbose_result(e);
    update_success_tokens(e);
  }

  void handle_stop_event(const StopEvent &e) {
    std::string_view type_name = "Unknown";
    std::string activity_text;
    bool build_success = false;

    if (auto it = activities_.find(e.id); it != activities_.end()) {
      type_name = NixLogParser::activity_type_name(it->second.type);
      if (it->second.type == ActivityType::Build && success_tokens_ > 0) {
        build_success = true;
        --success_tokens_;
      }
      activity_text = it->second.text;
      activities_.erase(it);
    }

    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), "{} {}",
                   fmt::styled("[stop]", fmt::fg(fmt::terminal_color::red)),
                   type_name);
    if (build_success) {
      fmt::format_to(std::back_inserter(buf), " {}",
                     fmt::styled("OK", fmt::fg(fmt::terminal_color::green)));
    }
    if (!activity_text.empty()) {
      fmt::format_to(std::back_inserter(buf), " {}", activity_text);
    }
    fmt::format_to(std::back_inserter(buf), "\n");
    fmt::print("{}", fmt::to_string(buf));
  }

  void handle_msg_event(const MsgEvent &e) {
    fmt::print("{} {}\n",
               fmt::styled("[msg]", fmt::fg(fmt::terminal_color::cyan)), e.msg);
  }

  void update_success_tokens(const ResultEvent &e) {
    if (e.type != ResultType::Progress || !builds_activity_ ||
        *builds_activity_ != e.id) {
      return;
    }
    if (auto done = int_field(e, 0)) {
      if (*done > last_progress_done_) {
        success_tokens_ += (*done - last_progress_done_);
        last_progress_done_ = *done;
      }
    }
  }

  void print_verbose_result(const ResultEvent &e) {
    fmt::memory_buffer buf;
    fmt::format_to(
        std::back_inserter(buf), "{} {}",
        fmt::styled("[result]", fmt::fg(fmt::terminal_color::yellow)),
        NixLogParser::result_type_name(e.type));

    auto append_str = [&](const char *label, size_t idx) {
      if (auto value = string_field(e, idx)) {
        fmt::format_to(std::back_inserter(buf), " {}=\"{}\"", label, *value);
      }
    };

    auto append_int = [&](const char *label, size_t idx) {
      if (auto value = int_field(e, idx)) {
        fmt::format_to(std::back_inserter(buf), " {}={}", label, *value);
      }
    };

    switch (e.type) {
    case ResultType::BuildLogLine:
    case ResultType::PostBuildLogLine:
    case ResultType::FetchStatus:
      fmt::format_to(std::back_inserter(buf), " ");
      append_str("msg", 0);
      break;
    case ResultType::SetPhase:
      fmt::format_to(std::back_inserter(buf), " ");
      append_str("phase", 0);
      break;
    case ResultType::Progress:
      fmt::format_to(std::back_inserter(buf), " ");
      append_int("done", 0);
      append_int("expected", 1);
      append_int("running", 2);
      append_int("failed", 3);
      break;
    case ResultType::SetExpected:
      append_int("activity_type", 0);
      append_int("expected", 1);
      break;
    case ResultType::FileLinked:
      append_int("done", 0);
      append_int("total", 1);
      break;
    case ResultType::UntrustedPath:
    case ResultType::CorruptedPath:
      fmt::format_to(std::back_inserter(buf), " ");
      append_str("path", 0);
      break;
    default:
      break;
    }

    fmt::format_to(std::back_inserter(buf), "\n");
    fmt::print("{}", fmt::to_string(buf));
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
