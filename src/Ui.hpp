#pragma once

#include "UiTypes.hpp"
#include <fmt/core.h>
#include <memory>
#include <string_view>

namespace nixb {

// Forward declarations
class UiSession;

// Backend interface for UI rendering
class UiBackend {
public:
  virtual ~UiBackend() = default;

  // Append a line to the log stream
  virtual void println(std::string_view line) = 0;

  // Update the HUD with a new state snapshot
  virtual void update_hud(const UiState &state) = 0;

  // Whether terminal features are active
  virtual bool enabled() const = 0;
};

// Log stream for append-only output (top region)
class LogStream {
public:
  // Constructor (only for use by UiSession)
  explicit LogStream(UiBackend &backend) : backend_(backend) {}

  // Print a line (adds newline if needed)
  void println(std::string_view line);

  // Printf-style formatted output
  template <typename... Args>
  void printf(fmt::format_string<Args...> fmt_str, Args &&...args) {
    println(fmt::format(fmt_str, std::forward<Args>(args)...));
  }

private:
  UiBackend &backend_;
};

// Activity HUD for dynamic status display (bottom region)
class ActivityHud {
public:
  // Constructor (only for use by UiSession)
  explicit ActivityHud(UiBackend &backend) : backend_(backend) {}

  // Present a complete new snapshot of the activity HUD
  void present(const UiState &state);

private:
  UiBackend &backend_;
  UiState last_state_;
};

// Main UI session facade
class UiSession {
public:
  // Create a UI session
  // - force: if true, attempt to use terminal features even when not a TTY
  static UiSession create(bool force = false);

  // Append-only log stream (top region)
  LogStream &log() { return *log_; }

  // Dynamic HUD view (bottom region)
  ActivityHud &hud() { return *hud_; }

  // Whether interactive terminal features are actually enabled
  bool enabled() const { return backend_ && backend_->enabled(); }

  // RAII teardown: restores terminal state
  ~UiSession();

  // Movable, non-copyable
  UiSession(UiSession &&) noexcept = default;
  UiSession &operator=(UiSession &&) noexcept = default;

  UiSession(const UiSession &) = delete;
  UiSession &operator=(const UiSession &) = delete;

private:
  explicit UiSession(std::unique_ptr<UiBackend> backend);

  std::unique_ptr<UiBackend> backend_;
  std::unique_ptr<LogStream> log_;
  std::unique_ptr<ActivityHud> hud_;
};

} // namespace nixb
