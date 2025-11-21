#pragma once

#include "NixBuildState.hpp"
#include "NixLogParser.hpp"
#include "NixLogRecorder.hpp"
#include "TerminalUi.hpp"

#include <atomic>
#include <cstdint>
#include <fmt/color.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
                          = std::nullopt,
                          std::atomic<bool> *stop_flag = nullptr);

  void process_input ();
  void process_playback_file (const std::string &path);
  void process_playback_file (const std::string &path, double speedup);

private:
  void process_line (const std::string &line);
  void handle_start_event (const StartEvent &e);
  void handle_result_event (const ResultEvent &e);
  void handle_stop_event (const StopEvent &e);
  void handle_msg_event (const MsgEvent &e);
  void emit_log (const std::string &block);
  void refresh_ui ();
  void rebuild_ui_state ();
  std::optional<std::string> format_activity_log_line (
      std::string_view prefix, fmt::terminal_color color, int64_t id,
      std::optional<int64_t> parent, const ActivityInfo &info,
      const std::function<std::string (const ActivityInfo &)> &label_fn) const;
  bool
  stop_requested () const
  {
    return stop_flag_ && stop_flag_->load (std::memory_order_relaxed);
  }

  bool quiet_;
  NixLogParser parser_;
  std::shared_ptr<nix::Store> store_;
  std::unique_ptr<NixBuildState> state_;
  std::unique_ptr<NixLogRecorder> recorder_;
  std::unique_ptr<TerminalUi> ui_;
  UiState ui_state_;
  std::atomic<bool> *stop_flag_ = nullptr;
};

} // namespace nixb
