#pragma once

#include "NixBuildState.hpp"
#include "NixLogParser.hpp"
#include "NixLogRecorder.hpp"
#include "Ui.hpp"

#include <atomic>
#include <fmt/color.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace nix
{
class Store;
class EvalState;
template <typename T> class ref;
} // namespace nix

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
                          std::atomic<bool> *stop_flag = nullptr,
                          double emit_delay_ms = 0.0);
  ~NixLogWatcher ();

  // Evaluate an installable and return derivation JSON strings.
  std::vector<std::string>
  show_derivation (const std::string &installable);

  void process_input ();
  void process_playback_file (const std::string &path);
  void process_playback_file (const std::string &path, double speedup);
  void
  process_log_line (const std::string &line)
  {
    process_line (line);
  }
  void finish ();

private:
  void process_line (const std::string &line);
  void handle_start_event (const StartEvent &e);
  void handle_result_event (const ResultEvent &e);
  void handle_stop_event (const StopEvent &e);
  void handle_msg_event (const MsgEvent &e);
  void emit_log (const std::string &block);
  void refresh_ui ();
  void rebuild_ui_state ();
  void render_loop ();
  bool
  stop_requested () const
  {
    return stop_flag_ && stop_flag_->load (std::memory_order_relaxed);
  }

  // Lazily initialize and return the eval state
  nix::ref<nix::EvalState> get_eval_state ();

  bool quiet_;
  NixLogParser parser_;
  std::shared_ptr<nix::Store> store_;
  std::unique_ptr<NixBuildState> state_;
  std::unique_ptr<NixLogRecorder> recorder_;
  UiSession ui_;
  UiState ui_state_;
  double emit_delay_ms_{ 0.0 };
  std::atomic<bool> *stop_flag_ = nullptr;
  std::optional<int> pending_derivation_count_;

  // Eval state (lazily initialized)
  std::shared_ptr<nix::EvalState> eval_state_;

  // Render thread for continuous UI updates
  std::unique_ptr<std::thread> render_thread_;
  std::mutex state_mutex_;
  size_t last_rendered_generation_ = 0; // Cache invalidation
};

} // namespace nixb
