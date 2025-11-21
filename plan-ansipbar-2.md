Yeah, it’s pretty doable, with a couple of caveats about how scroll regions work.

### 0. Terminal constraint you have to respect

The key thing: the ANSI scroll region (`CSI top;bottom r`) takes **fixed** row numbers. There’s no “auto-grow” footer. So if you want a variable-height bottom area, you just:

- Recompute how tall you want the footer to be, and
- Reset the scroll region to end just above it, and
- Redraw the footer.

That’s it. No alt buffer involved, scrollback still works.

So “dynamic size of the bottom progress part” = “occasionally recompute `status_lines_` and call `set_scroll_region()` again”.

---

## 1. Dynamic footer lines: the rough shape

Say we want:

- One line per _currently active build_ (however you define “active”: started but not stopped, or running vs queued), plus maybe:

  - one header/aggregate line, and
  - one line for downloads, etc.

Then:

```cpp
int desired_status_lines = 0;

if (!ui_state_.builds_per_activity.empty()) {
    desired_status_lines += 1; // header
    desired_status_lines += ui_state_.builds_per_activity.size();
}

if (ui_state_.transfers) {
    desired_status_lines += 1;
}

// Always force at least 1 (or 0) depending on taste.
desired_status_lines = std::clamp(desired_status_lines, 1, rows_-1);
```

Whenever `desired_status_lines` changes, we reconfigure the scroll region and redraw.

---

## 2. How to wire that into the `TerminalUi` class

Building on the previous design, you add an internal “set status lines” method:

```cpp
class TerminalUi {
public:
  explicit TerminalUi(int initial_status_lines = 3, bool force = false);

  void print_log_block(std::string_view block);
  void redraw(const UiState &state);

  // New: allow dynamic footer height
  void update_status_height(int desired_status_lines, const UiState &state);

private:
  void reconfigure_scroll_region();

  bool enabled_ = false;
  int rows_ = 0;
  int cols_ = 0;

  int status_lines_ = 0;
  int scroll_bottom_ = 0;

  UiState last_state_; // keep a copy so we can redraw after resize
};
```

Implementation sketch:

```cpp
namespace {
constexpr const char *CSI = "\x1b[";
}

TerminalUi::TerminalUi(int initial_status_lines, bool force) {
  // same as before: isatty/TERM check, ioctl(TIOCGWINSZ), etc
  // compute rows_, cols_

  status_lines_ = std::clamp(initial_status_lines, 1, rows_ - 1);
  reconfigure_scroll_region();
  enabled_ = true;
}

void TerminalUi::reconfigure_scroll_region() {
  scroll_bottom_ = rows_ - status_lines_;
  if (scroll_bottom_ < 1) scroll_bottom_ = 1;

  // Set scroll region for the log area.
  fmt::print("{}1;{}r", CSI, scroll_bottom_);
  // Put cursor at top of log region so further logs scroll correctly.
  fmt::print("{}1;1H", CSI);
  std::fflush(stdout);
}

void TerminalUi::update_status_height(int desired_status_lines,
                                      const UiState &state) {
  if (!enabled_) return;

  desired_status_lines = std::clamp(desired_status_lines, 1, rows_ - 1);
  if (desired_status_lines == status_lines_) {
    // height unchanged, just redraw with new state
    last_state_ = state;
    redraw(last_state_);
    return;
  }

  status_lines_ = desired_status_lines;
  reconfigure_scroll_region();

  // Now the footer occupies a different number of lines; redraw everything.
  last_state_ = state;
  redraw(last_state_);
}
```

Now `redraw()` works exactly like before, just using `status_lines_` to find the first footer row:

```cpp
void TerminalUi::redraw(const UiState &state) {
  if (!enabled_) return;

  int first_status_row = rows_ - status_lines_ + 1;
  int row = first_status_row;

  auto draw_line = [&](std::string_view text) {
    fmt::print("{}{};1H{}2K{}", CSI, row, CSI, text);
    ++row;
  };

  // e.g. first line: global builds summary
  if (state.builds_aggregate) {
    draw_line(render_progress_line("[builds]", *state.builds_aggregate, cols_));
  } else {
    draw_line("[builds] waiting…");
  }

  // Then one line per currently active build
  for (const auto &b : state.active_builds) {
    // b could have fields: drv_name, status, maybe per-build fraction if you have it
    draw_line(render_single_build_line(b, cols_));
  }

  // Maybe a download/progress line
  if (state.transfers) {
    draw_line(render_progress_line("[downloads]", *state.transfers, cols_));
  }

  // If we have leftover footer rows (because we rounded up), clear them:
  while (row <= rows_) {
    draw_line("");
  }

  // Leave cursor on last status line
  fmt::print("{}{};1H", CSI, rows_);
  std::fflush(stdout);
}
```

So the _core behavior_ is:

1. `update_status_height()` decides how many rows the footer should use.
2. It resets the scroll region.
3. It calls `redraw()` to repaint the footer in its new height.

Scrollback is unaffected; all those top lines you scrolled out remain in the terminal history.

---

## 3. Where “dynamic” happens: inside `NixLogWatcher`

You already maintain:

- `activities_ : id -> ActivityInfo{type, text}`
- `builds_activity_`
- `success_tokens_`, `last_progress_done_`

You can extend `UiState` with a vector of active builds:

```cpp
struct SingleBuildState {
  int64_t id;
  std::string label;      // maybe shortened drv / store path
  std::string status;     // "queued", "running", "done", "failed"...
};

struct UiState {
  std::optional<ActivityProgress> builds_aggregate;
  std::optional<ActivityProgress> transfers;
  std::string current_phase;
  std::vector<SingleBuildState> active_builds;
};
```

Then somewhere in `update_progress` or in a `rebuild_ui_state()` helper, you derive:

```cpp
int desired_status_lines = 0;

// one header for builds aggregate
if (ui_state_.builds_aggregate) {
  desired_status_lines += 1;
}

// one per active build (maybe cap it to prevent footer eating the whole screen)
int max_build_lines = std::max(3, rows_/2); // arbitrary policy
int build_lines = std::min<int>(ui_state_.active_builds.size(), max_build_lines);
desired_status_lines += build_lines;

// optional extra line for downloads
if (ui_state_.transfers) {
  desired_status_lines += 1;
}

// Maybe ensure at least 2 lines so it's not cramped.
desired_status_lines = std::max(desired_status_lines, 2);

ui_->update_status_height(desired_status_lines, ui_state_);
```

So every:

- new `StartEvent` for a build
- `StopEvent` for a build
- or progress event that changes the vector

can trigger a potential size change. In practice, that’s just one extra `CSI 1;{bottom}r` + redraw; terminals handle this fine.

---

## 4. Trade-offs & practical considerations

- **Flicker**: resizing the footer on every single “started another build” event will cause visible jumps at the bottom. It’s usually acceptable, but you can:

  - Debounce (only resize at most once every X ms), or
  - Clamp to a max footer size so it doesn’t grow without bound.

- **Max footer size**: you probably don’t want 30 lines of footer on a 40-line terminal. A good compromise:

  - `max_status_lines = rows / 3` or `rows / 2`
  - Show `min(active_builds, max_status_lines - (header + downloads))`

- **Per-build progress**: Nix’s `resProgress` for `actBuilds` is aggregate; there isn’t rich per-drv progress. So your per-build lines might be:

  - just status (`queued`, `running`, `done`, `failed`);
  - maybe order them by `start`/`stop` time; and
  - use a simple spinner or tick mark rather than a real percentage.

From the terminal mechanics side, though, “one line per current build” is absolutely compatible with the scroll-region, no-alt-buffer design. It really is just: _recompute footer height → reset scroll region → repaint footer_, and keep all ANSI nastiness inside the `TerminalUi` helper.
