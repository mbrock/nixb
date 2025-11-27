#include <sys/ioctl.h>
#include <unistd.h>
#include <csignal>
#include <iostream>

#include "nxt/app.hpp"
#include "nxt/ansi.hpp"
#include "nxt/async.hpp"
#include "nxt/units.hpp"

namespace nxb::ui {

UIRuntime::UIRuntime()
    : scheduler_(
          nxb::io_scheduler::make_shared(nxb::io_scheduler::options{}))
{
    signals_.watch(SIGINT, SIGTERM, SIGWINCH);
    refresh_terminal_size();

    // Create compositor with initial terminal size
    compositor_ =
        std::make_unique<TerminalCompositor>(terminal_size(), glyphs_);
}

UIRuntime::~UIRuntime() = default;

void UIRuntime::refresh_terminal_size() noexcept
{
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0
        && ws.ws_row > 0) {
        term_width_.store(ws.ws_col * ch, std::memory_order_release);
        term_height_.store(ws.ws_row * ln, std::memory_order_release);
    }
}

void UIRuntime::request_shutdown()
{
    if (stop_source_.stop_requested())
        return; // Already shutting down

    stop_source_.request_stop();
    damage_event_.set();   // Wake present_loop
    SignalPipe::notify(0); // Wake signal_loop
}

void UIRuntime::signal_damage()
{
    damage_counter_.fetch_add(1, std::memory_order_acq_rel);
    damage_event_.set();
}

TermSize UIRuntime::terminal_size() const noexcept
{
    return TermSize{terminal_width(), terminal_height()};
}

nxb::width_t UIRuntime::terminal_width() const noexcept
{
    return term_width_.load(std::memory_order_acquire);
}

nxb::height_t UIRuntime::terminal_height() const noexcept
{
    return term_height_.load(std::memory_order_acquire);
}

void UIRuntime::render_impl(
    std::function<void(RasterView &, Size)> render_fn)
{
    auto & buffer = compositor_->back_buffer();
    buffer.clear();
    auto view = buffer.view();
    auto size = compositor_->size();
    render_fn(view, size);
    compositor_->present_frame();
}

void UIRuntime::update_hud_height(height_t hud_h)
{
    compositor_->set_hud_height(hud_h, terminal_height());
}

void UIRuntime::println(std::string_view line)
{
    auto hud_h = compositor_->hud_height();
    auto term_h = terminal_height();

    // No scroll region in full-screen mode
    if (hud_h >= term_h)
        return;

    // Just print - cursor is already in the scroll region.
    // The scroll region handles scrolling automatically when we hit the
    // bottom.
    fmt::memory_buffer buf;
    ansi::Writer w(buf);
    w.reset(); // Avoid HUD styling leaking into log output
    w.text(line);
    w.clear_line_from_cursor();
    w.text("\n");

    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

void UIRuntime::cleanup()
{
    auto hud_h = compositor_->hud_height();
    auto term_h = terminal_height();

    // Nothing to clear in full-screen mode
    if (hud_h >= term_h)
        return;

    // Clear HUD region plus spacer row above, preserving cursor position
    fmt::memory_buffer buf;
    ansi::Writer w(buf);
    w.save_cursor();

    // Clear from HUD area to bottom using raw row numbers to avoid coord
    // issues
    auto term_rows = static_cast<int>(term_h.numerical_value_in(ln));
    auto hud_rows = static_cast<int>(hud_h.numerical_value_in(ln));
    auto start_row =
        std::max(0, term_rows - hud_rows - 1); // extra row for spacer
    // Clear rows start_row through term_rows-1 (0-indexed)
    for (int r = start_row; r < term_rows; ++r) {
        w.move_to(Pos::at(0 * ch, r * ln));
        w.clear_line();
    }

    w.restore_cursor();
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

TerminalCompositor & UIRuntime::compositor() noexcept
{
    return *compositor_;
}

nxb::task<> UIRuntime::signal_loop()
{
    while (!shutdown_requested()) {
        // Poll the signal pipe for readability
        co_await scheduler_->poll(signals_.read_fd(), nxb::poll_op::read);

        // Drain all pending signals
        while (auto sig = signals_.try_read()) {
            switch (*sig) {
            case 0: // Internal shutdown request (normal completion)
                signal_damage();
                co_return;

            case SIGINT:
            case SIGTERM:
                request_shutdown();
                co_return;

            case SIGWINCH:
                refresh_terminal_size();
                co_await resize_queue_.push(terminal_size());
                signal_damage();
                break;

            default:
                break;
            }
        }
    }

    co_return;
}

} // namespace nxb::ui
