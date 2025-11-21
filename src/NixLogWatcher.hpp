#pragma once

#include "IdColor.hpp"
#include "NixLogParser.hpp"
#include "TerminalUi.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <nix/store/path.hh>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace nix
{
class Store;
}

namespace nixb
{

class NixLogWatcher
{
public:
  enum class UiMode
  {
    Auto,
    Off,
    On
  };

  explicit NixLogWatcher (bool quiet, UiMode ui_mode = UiMode::Auto,
                          std::optional<std::string> record_path
                          = std::nullopt);

  void process_input ();
  void process_playback_file (const std::string &path);
  void process_playback_file (const std::string &path, double speedup);

private:
  struct ActivityInfo
  {
    ActivityType type;
    std::string text;
    std::optional<nix::StorePath> store_path;
    std::optional<std::string> store_base_url;
    std::string label;
    std::optional<int64_t> parent;
  };

  void process_line (const std::string &line);
  void handle_start_event (const StartEvent &e);
  void handle_result_event (const ResultEvent &e);
  void handle_stop_event (const StopEvent &e);
  void handle_msg_event (const MsgEvent &e);
  void emit_log (const std::string &block);
  void update_success_tokens (const ResultEvent &e);
  void update_progress (const ResultEvent &e);
  void record_line (const std::string &line);
  void refresh_ui ();
  void rebuild_active_builds ();
  void rebuild_active_transfers ();
  void note_transfer_start (int64_t id);
  void note_transfer_stop (int64_t id);
  std::string format_activity_label (const ActivityInfo &info) const;
  std::optional<std::string> format_activity_log_line (
      std::string_view prefix, fmt::terminal_color color, int64_t id,
      std::optional<int64_t> parent, const ActivityInfo &info,
      const std::function<std::string (const ActivityInfo &)> &label_fn) const;

  bool quiet_;
  NixLogParser parser_;
  std::shared_ptr<nix::Store> store_;
  std::unordered_map<int64_t, ActivityInfo> activities_;
  std::optional<int64_t> builds_activity_;
  int64_t success_tokens_ = 0;
  int64_t last_progress_done_ = 0;
  std::unordered_map<int64_t, ActivityProgress> transfer_progress_;

  std::unique_ptr<TerminalUi> ui_;
  UiState ui_state_;

  bool recording_enabled_ = false;
  std::ofstream record_stream_;
  std::chrono::steady_clock::time_point start_time_;
  bool start_time_set_ = false;

  std::unordered_set<int64_t> active_transfers_;
};

} // namespace nixb
