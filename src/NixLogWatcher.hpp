#pragma once

#include "NixLogParser.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace nix {
class Store;
}

namespace nixb {

class NixLogWatcher {
public:
  explicit NixLogWatcher(bool quiet);

  void process_input();

private:
  struct ActivityInfo {
    ActivityType type;
    std::string text;
  };

  void process_line(const std::string &line);
  void handle_start_event(const StartEvent &e);
  void handle_result_event(const ResultEvent &e);
  void handle_stop_event(const StopEvent &e);
  void handle_msg_event(const MsgEvent &e);
  void update_success_tokens(const ResultEvent &e);

  bool quiet_;
  NixLogParser parser_;
  std::shared_ptr<nix::Store> store_;
  std::unordered_map<int64_t, ActivityInfo> activities_;
  std::optional<int64_t> builds_activity_;
  int64_t success_tokens_ = 0;
  int64_t last_progress_done_ = 0;
};

} // namespace nixb

