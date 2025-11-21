Here’s a design that keeps your codebase recognizable, adds a “bottom progress bars” UI, and still behaves like a normal filter with scrollback (no alt buffer, just ANSI cursor tricks).

---

## 1. High‑level shape

Right now you basically have:

- `NixLogParser` → turns `@nix {…}` JSON lines into `StartEvent / ResultEvent / StopEvent / MsgEvent`.
- `NixLogWatcher` → reads stdin, parses, and `fmt::print()`s formatted text.
- `main` → sets up CLI and creates `NixLogWatcher`.

Let’s factor this into three layers:

1. **Model** – understanding Nix events and tracking progress (`done/expected/running/failed` etc).
2. **View** – terminal rendering (plain vs “bottom progress bars”).
3. **Controller** – `NixLogWatcher` tying parser + model + view together.

The goal is:

- If stdout is a TTY (and not `TERM=dumb`), we enable an **ANSI UI**:

  - A scrollable log area.
  - A fixed status area at the bottom with progress bars.

- If stdout is not a TTY **or** the user passes `--no-ui`, we behave exactly like now.

No alt screen, scrollback works because we only use a scroll region and in-place redraws, not a separate buffer.

---

## 2. Terminal layout & ANSI strategy

### 2.1. Split screen using scroll regions

We can get “bottom lines stay fixed while top part scrolls” with the standard DECSTBM scroll region:

- Query terminal size (rows/cols) via `ioctl(TIOCGWINSZ)`.
- Decide how many status lines we want, say `status_lines = 3`.
- Compute:

```text
rows = ws.ws_row
cols = ws.ws_col
scroll_bottom = rows - status_lines   // log area bottom row
```

- Set scroll region for the log area:

```text
CSI 1;{scroll_bottom}r
```

(where `CSI` is `"\x1b["`)

Now:

- Any `\n` printed **while the cursor is in rows 1..scroll_bottom** will scroll only that region.
- The bottom `status_lines` rows stay “fixed” until we explicitly overwrite them.

Scrollback remains intact: lines scrolled out of the top of the scroll region go to history as usual.

### 2.2. How to print logs and keep bars at the bottom

We’ll centralize all “print this block of log text” calls into the UI object.

For logs, in interactive mode:

1. Move the cursor to the **bottom line of the log region** (row `scroll_bottom`, col 1).
2. Clear that line.
3. Print the block (which may contain `\n`).

Pseudo:

```cpp
fmt::print("\x1b[{};1H\x1b[2K", scroll_bottom_);
fmt::print("{}", block);
std::fflush(stdout);
```

Because we are at the bottom of the scroll region:

- The _first_ newline in the block scrolls the region by one line.
- Your log lines become part of the scrollback.
- The status area (rows `scroll_bottom+1 .. rows`) is unaffected.

There _will_ be a single blank line between the log area and the status area, which actually makes a nice visual separator.

For progress bars:

- We never print `\n` when redrawing them.
- Instead we directly position the cursor on those bottom lines and overwrite them in place.

Example:

```cpp
int first_status_row = rows_ - status_lines_ + 1;
int row = first_status_row;

auto draw_line = [&](std::string_view text) {
    fmt::print("\x1b[{};1H\x1b[2K{}", row, text);
    ++row;
};
```

No new lines → no scrolling → bars stay pinned, no pollution in scrollback.

### 2.3. RAII terminal controller

Encapsulate ANSI nastiness in **one class**, so the rest of the code just says “print block” or “redraw status”.

```cpp
// src/TerminalUi.hpp
#pragma once

#include <optional>
#include <string>

namespace nixb {

struct ActivityProgress {
  int64_t done   = 0;
  int64_t expected = 0;
  int64_t running  = 0;
  int64_t failed   = 0;
};

struct UiState {
  std::optional<ActivityProgress> builds;
  std::optional<ActivityProgress> transfers;
  std::string current_phase; // e.g. "configure", "build", etc.
};

class TerminalUi {
public:
  explicit TerminalUi(int status_lines = 3, bool force = false);
  ~TerminalUi();

  bool enabled() const { return enabled_; }

  // Append a scrolling block of text (normal log output).
  void print_log_block(std::string_view block);

  // Redraw bottom progress bars from the current UiState.
  void redraw(const UiState &state);

private:
  bool enabled_ = false;
  int status_lines_ = 0;
  int rows_ = 0;
  int cols_ = 0;
  int scroll_bottom_ = 0;
};

} // namespace nixb
```

Implementation sketch:

```cpp
// src/TerminalUi.cpp
#include "TerminalUi.hpp"

#include <fmt/core.h>
#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

namespace nixb {

namespace {
constexpr const char *CSI = "\x1b[";

std::string render_progress_line(std::string_view label,
                                 const ActivityProgress &p,
                                 int cols) {
  // Avoid division by zero and weird totals.
  int64_t total = p.done + p.expected + p.running + p.failed;
  if (total <= 0) {
    return fmt::format("{} waiting…", label);
  }

  double frac = static_cast<double>(p.done) / static_cast<double>(total);

  // Reserve some columns for numbers and label.
  int reserved = static_cast<int>(label.size()) + 20;
  int bar_width = std::max(10, cols - reserved);
  int filled = static_cast<int>(frac * bar_width + 0.5);

  fmt::memory_buffer buf;
  fmt::format_to(buf, "{}", label);
  fmt::format_to(buf, " [");
  for (int i = 0; i < bar_width; ++i) {
    fmt::format_to(buf, "{}", i < filled ? '#' : '-');
  }
  fmt::format_to(buf, "]");
  fmt::format_to(buf, " {}/{}", p.done, total);
  if (p.running > 0 || p.failed > 0) {
    fmt::format_to(buf, " ({} running, {} failed)", p.running, p.failed);
  }

  return fmt::to_string(buf);
}

} // namespace

TerminalUi::TerminalUi(int status_lines, bool force)
    : status_lines_(status_lines) {
  if (!force) {
    if (!::isatty(STDOUT_FILENO)) return;
    const char *term = std::getenv("TERM");
    if (!term || std::string_view(term) == "dumb") return;
  }

  winsize ws{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
      ws.ws_row == 0 || ws.ws_col == 0) {
    return;
  }

  rows_ = ws.ws_row;
  cols_ = ws.ws_col;

  if (rows_ <= status_lines_) {
    return;
  }

  scroll_bottom_ = rows_ - status_lines_;

  // Restrict scrolling to the log area.
  fmt::print("{}1;{}r", CSI, scroll_bottom_);
  // Start at top of log area.
  fmt::print("{}1;1H", CSI);
  std::fflush(stdout);

  enabled_ = true;
}

TerminalUi::~TerminalUi() {
  if (!enabled_) return;

  // Reset scroll region to full screen and move to the line after the UI.
  fmt::print("{}r", CSI);    // clear scroll region
  fmt::print("\n");          // leave a newline after the UI
  std::fflush(stdout);
}

void TerminalUi::print_log_block(std::string_view block) {
  if (!enabled_) {
    fmt::print("{}", block);
    return;
  }

  // Jump to bottom line of the scroll region and clear it.
  fmt::print("{}{};1H{}2K", CSI, scroll_bottom_, CSI);
  fmt::print("{}", block);
  std::fflush(stdout);
}

void TerminalUi::redraw(const UiState &state) {
  if (!enabled_) return;

  int first_status_row = rows_ - status_lines_ + 1;
  int row = first_status_row;

  auto draw_line = [&](std::string_view text) {
    fmt::print("{}{};1H{}2K{}", CSI, row, CSI, text);
    ++row;
  };

  if (state.builds) {
    draw_line(render_progress_line("[builds]", *state.builds, cols_));
  } else {
    draw_line("[builds] waiting…");
  }

  if (state.transfers) {
    draw_line(render_progress_line("[downloads]", *state.transfers, cols_));
  } else {
    draw_line("");
  }

  if (!state.current_phase.empty()) {
    draw_line(state.current_phase);
  } else {
    draw_line("");
  }

  // Leave cursor on the last status line so the user sees it “at the bottom”.
  fmt::print("{}{};1H", CSI, rows_);
  std::fflush(stdout);
}

} // namespace nixb
```

All ANSI is confined here; if you ever want a different layout, you only touch this file.

---

## 3. Turning Nix events into progress bars

Next we need a little **progress model** inside `NixLogWatcher` that feeds `UiState`.

You already track:

- `activities_ : id -> {ActivityType, text}`
- `builds_activity_`
- `success_tokens_`, `last_progress_done_`

We just extend with progress snapshots:

```cpp
// NixLogWatcher.hpp
struct ActivityProgress {
  int64_t done = 0;
  int64_t expected = 0;
  int64_t running = 0;
  int64_t failed = 0;
};

struct UiState {
  std::optional<ActivityProgress> builds;
  std::optional<ActivityProgress> transfers;
  std::string current_phase;
};
```

And hold:

```cpp
std::unique_ptr<TerminalUi> ui_;
UiState ui_state_;
```

### 3.1. Updating progress when results arrive

In `NixLogWatcher.cpp`, add:

```cpp
void NixLogWatcher::update_progress(const ResultEvent &e) {
  if (!ui_ || !ui_->enabled()) return;

  auto it = activities_.find(e.id);
  if (it == activities_.end()) {
    // Some results (like top-level, maybe) might not have a start.
    if (e.type == nix::resSetPhase) {
      if (auto phase = e.get_string(0)) {
        ui_state_.current_phase = fmt::format("[phase] {}", *phase);
      }
    }
    return;
  }

  ActivityType activity_type = it->second.type;

  if (e.type == nix::resProgress) {
    ActivityProgress p;
    if (auto v = e.get_int(0)) p.done    = *v;
    if (auto v = e.get_int(1)) p.expected = *v;
    if (auto v = e.get_int(2)) p.running  = *v;
    if (auto v = e.get_int(3)) p.failed   = *v;

    switch (activity_type) {
    case nix::actBuilds:
      ui_state_.builds = p;
      break;
    case nix::actFileTransfer:
    case nix::actCopyPaths:
      ui_state_.transfers = p;
      break;
    default:
      break;
    }
  } else if (e.type == nix::resSetPhase) {
    if (auto phase = e.get_string(0)) {
      ui_state_.current_phase = fmt::format("[phase] {}", *phase);
    }
  }
}
```

You already have `update_success_tokens(const ResultEvent&)`; we just call `update_progress(e)` alongside it.

You can refine this later to handle `resSetExpected` more faithfully; the design keeps the mapping logic localized.

---

## 4. Wiring the UI into `NixLogWatcher`

### 4.1. Constructor

In `NixLogWatcher.hpp`:

```cpp
#include "TerminalUi.hpp"

class NixLogWatcher {
public:
  enum class UiMode { Auto, Off, On };

  explicit NixLogWatcher(bool quiet, UiMode ui_mode = UiMode::Auto);
  ...
private:
  void emit_log(const std::string &block);
  void update_progress(const ResultEvent &e);
  ...
  bool quiet_;
  NixLogParser parser_;
  std::shared_ptr<nix::Store> store_;
  std::unordered_map<int64_t, ActivityInfo> activities_;
  std::optional<int64_t> builds_activity_;
  int64_t success_tokens_ = 0;
  int64_t last_progress_done_ = 0;

  std::unique_ptr<TerminalUi> ui_;
  UiState ui_state_;
};
```

In `NixLogWatcher.cpp`:

```cpp
NixLogWatcher::NixLogWatcher(bool quiet, UiMode ui_mode)
    : quiet_(quiet) {
  nix::initLibStore();
  store_ = nix::openStore();

  // This diagnostic probably shouldn't go through the UI; keep it on stderr.
  fmt::print(stderr, "Yay! Nix store opened successfully: {}\n",
             store_->config.getHumanReadableURI());

  if (ui_mode != UiMode::Off) {
    bool force = (ui_mode == UiMode::On);
    auto ui = std::make_unique<TerminalUi>(3, force);
    if (ui->enabled()) {
      ui_ = std::move(ui);
    }
  }
}
```

### 4.2. One place to print logs

Centralize log printing:

```cpp
void NixLogWatcher::emit_log(const std::string &block) {
  if (ui_ && ui_->enabled()) {
    ui_->print_log_block(block);
    ui_->redraw(ui_state_);
  } else {
    fmt::print("{}", block);
  }
}
```

Then in the handlers:

```cpp
void NixLogWatcher::handle_start_event(const StartEvent &e) {
  activities_[e.id] = ActivityInfo{e.type, e.text};
  if (e.type == nix::actBuilds) {
    builds_activity_ = e.id;
  }
  emit_log(e.format());
}

void NixLogWatcher::handle_result_event(const ResultEvent &e) {
  emit_log(e.format());
  update_success_tokens(e);
  update_progress(e);
  if (ui_ && ui_->enabled()) {
    ui_->redraw(ui_state_);
  }
}

void NixLogWatcher::handle_stop_event(const StopEvent &e) {
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

  emit_log(e.format(type_name, activity_text, build_success));
}

void NixLogWatcher::handle_msg_event(const MsgEvent &e) {
  emit_log(e.format());
}
```

And for non‑`@nix` lines:

```cpp
void NixLogWatcher::process_line(const std::string &line) {
  auto event_opt = parser_.parse_line(line);

  if (!event_opt) {
    if (!quiet_) {
      emit_log(fmt::format("{}\n", line));
    }
    return;
  }
  ...
}
```

Now _every_ bit of user-visible output flows through `emit_log`, which either goes:

- straight to `fmt::print` (plain mode), or
- through the scroll-region-aware `TerminalUi` (interactive mode).

---

## 5. CLI surface

Expose UI modes via CLI flags:

```cpp
// src/main.cpp
int main(int argc, char **argv) {
  CLI::App app{"nixb - minimal nix internal-json watcher"};

  bool quiet = false;
  app.add_flag("-q,--quiet", quiet,
               "suppress pass-through lines that are not @nix JSON");

  bool no_ui = false;
  bool force_ui = false;
  app.add_flag("--no-ui", no_ui, "Disable interactive progress UI");
  app.add_flag("--ui", force_ui, "Force interactive progress UI even if stdout is not a TTY");

  CLI11_PARSE(app, argc, argv);

  nixb::NixLogWatcher::UiMode ui_mode = nixb::NixLogWatcher::UiMode::Auto;
  if (no_ui) ui_mode = nixb::NixLogWatcher::UiMode::Off;
  else if (force_ui) ui_mode = nixb::NixLogWatcher::UiMode::On;

  nixb::NixLogWatcher watcher(quiet, ui_mode);
  watcher.process_input();
}
```

Typical usage:

- TTY: `nix build ... 2>&1 | ./build/nixb`

  - `stdout` of `nixb` is a TTY → Auto UI enabled.

- In CI: `nix build ... 2>&1 | ./build/nixb --no-ui`

  - Plain log output, no control sequences.

---

## 6. Implementation order (practical plan)

If you want to land this incrementally and keep things simple:

1. **Refactor `NixLogWatcher`** to use `emit_log()` everywhere but still print directly (no UI yet).
2. Add `TerminalUi` with:

   - Constructor that sets scroll region.
   - `print_log_block()` that just prints with the scroll-region trick.
   - No `UiState` yet; just stub `redraw()` to do nothing.

3. Add `UiState` and `update_progress()`:

   - Track only builds aggregate (`actBuilds` + `resProgress`) at first.
   - Draw a single `[builds]` bar.

4. Add downloads bar (`actFileTransfer` / `actCopyPaths`).
5. Add phase line from `resSetPhase`.
6. Optional: SIGWINCH handling to recompute `rows_` / `cols_`, but you can defer this.

This way the structure is “nice”:

- **Parser** stays untouched.
- **Watcher** has all the Nix semantics and glues to:
- **TerminalUi**, which owns all ANSI escape code logic and can be mocked or turned off.
