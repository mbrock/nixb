#include "tty-raster-diff.hpp"

#include <algorithm>

namespace nxb
{

namespace detail
{

std::optional<std::size_t>
find_next_diff_in_line (const DiffState &state, std::size_t y,
                        std::size_t start_x) noexcept
{
  if (y >= state.back->height () || start_x >= state.back->width ())
    return std::nullopt;

  const std::size_t line_start = y * state.back->width ();
  const std::size_t search_start = line_start + start_x;
  const std::size_t line_end = line_start + state.back->width ();

  const auto front_glyphs = state.front->glyphs ();
  const auto back_glyphs = state.back->glyphs ();
  const auto front_fgs = state.front->fgs ();
  const auto back_fgs = state.back->fgs ();
  const auto front_bgs = state.front->bgs ();
  const auto back_bgs = state.back->bgs ();

  std::optional<std::size_t> min_diff;

  // Find first glyph difference using std::mismatch (SIMD-friendly)
  auto [front_g, back_g]
      = std::mismatch (front_glyphs.begin () + search_start,
                       front_glyphs.begin () + line_end,
                       back_glyphs.begin () + search_start);

  if (front_g != front_glyphs.begin () + line_end)
    {
      std::size_t diff = std::distance (
          front_glyphs.begin () + search_start, front_g);
      min_diff = diff;
    }

  // Find first fg color difference
  auto [front_fg, back_fg]
      = std::mismatch (front_fgs.begin () + search_start,
                       front_fgs.begin () + line_end,
                       back_fgs.begin () + search_start);

  if (front_fg != front_fgs.begin () + line_end)
    {
      std::size_t diff
          = std::distance (front_fgs.begin () + search_start, front_fg);
      min_diff = min_diff ? std::min (*min_diff, diff) : std::optional (diff);
    }

  // Find first bg color difference
  auto [front_bg, back_bg]
      = std::mismatch (front_bgs.begin () + search_start,
                       front_bgs.begin () + line_end,
                       back_bgs.begin () + search_start);

  if (front_bg != front_bgs.begin () + line_end)
    {
      std::size_t diff
          = std::distance (front_bgs.begin () + search_start, front_bg);
      min_diff = min_diff ? std::min (*min_diff, diff) : std::optional (diff);
    }

  return min_diff ? std::optional (start_x + *min_diff) : std::nullopt;
}

std::size_t
find_run_end (const DiffState &state, std::size_t y, std::size_t start_x,
              std::optional<Rgba8> run_fg, std::optional<Rgba8> run_bg) noexcept
{
  const std::size_t line_start = y * state.back->width ();
  const std::size_t search_start = line_start + start_x + 1;
  const std::size_t line_end = line_start + state.back->width ();

  if (search_start >= line_end)
    return start_x + 1;

  std::size_t end_x = state.back->width (); // Default to end of line

  const auto back_fgs = state.back->fgs ();
  const auto back_bgs = state.back->bgs ();
  const auto default_color = Raster::DEFAULT_COLOR;

  // Find where foreground color changes
  if (run_fg.has_value () && *run_fg != default_color)
    {
      // Find first non-matching fg color
      auto it = std::find_if (
          back_fgs.begin () + search_start, back_fgs.begin () + line_end,
          [run_fg] (Rgba8 color) { return color != *run_fg; });

      if (it != back_fgs.begin () + line_end)
        {
          std::size_t pos
              = std::distance (back_fgs.begin () + line_start, it);
          end_x = std::min (end_x, pos);
        }
    }
  else if (!run_fg.has_value () || *run_fg == default_color)
    {
      // Run has default fg - find where it becomes non-default
      auto it = std::find_if (back_fgs.begin () + search_start,
                              back_fgs.begin () + line_end,
                              [] (Rgba8 color) {
                                return color != Raster::DEFAULT_COLOR;
                              });

      if (it != back_fgs.begin () + line_end)
        {
          std::size_t pos
              = std::distance (back_fgs.begin () + line_start, it);
          end_x = std::min (end_x, pos);
        }
    }

  // Find where background color changes
  if (run_bg.has_value () && *run_bg != default_color)
    {
      auto it = std::find_if (
          back_bgs.begin () + search_start, back_bgs.begin () + line_end,
          [run_bg] (Rgba8 color) { return color != *run_bg; });

      if (it != back_bgs.begin () + line_end)
        {
          std::size_t pos
              = std::distance (back_bgs.begin () + line_start, it);
          end_x = std::min (end_x, pos);
        }
    }
  else if (!run_bg.has_value () || *run_bg == default_color)
    {
      auto it = std::find_if (back_bgs.begin () + search_start,
                              back_bgs.begin () + line_end,
                              [] (Rgba8 color) {
                                return color != Raster::DEFAULT_COLOR;
                              });

      if (it != back_bgs.begin () + line_end)
        {
          std::size_t pos
              = std::distance (back_bgs.begin () + line_start, it);
          end_x = std::min (end_x, pos);
        }
    }

  return end_x;
}

} // namespace detail

coro::generator<ChangeRun>
diff_rasters (const Raster &front, const Raster &back)
{
  detail::DiffState state{ &front, &back, std::nullopt, std::nullopt };

  for (std::size_t y = 0; y < back.height (); ++y)
    {
      std::size_t x = 0;

      while (x < back.width ())
        {
          // Find next difference in this line
          auto diff_x = detail::find_next_diff_in_line (state, y, x);
          if (!diff_x)
            break; // No more diffs in this line

          x = *diff_x;
          const std::size_t idx = y * back.width () + x;

          // Determine run colors
          const auto run_fg = back.fgs ()[idx];
          const auto run_bg = back.bgs ()[idx];

          // Find end of color-consistent run
          const std::size_t end_x
              = detail::find_run_end (state, y, x, run_fg, run_bg);

          // Build ChangeRun
          ChangeRun run;
          run.x = x;
          run.y = y;
          run.glyphs = back.glyphs ().subspan (idx, end_x - x);

          // Determine color changes needed
          const auto default_color = Raster::DEFAULT_COLOR;

          // Foreground color logic
          if (run_fg == default_color && state.current_fg.has_value ())
            {
              run.fg_reset = true;
              state.current_fg = std::nullopt;
            }
          else if (run_fg != default_color && run_fg != state.current_fg)
            {
              run.fg_change = run_fg;
              state.current_fg = run_fg;
            }

          // Background color logic
          if (run_bg == default_color && state.current_bg.has_value ())
            {
              run.bg_reset = true;
              state.current_bg = std::nullopt;
            }
          else if (run_bg != default_color && run_bg != state.current_bg)
            {
              run.bg_change = run_bg;
              state.current_bg = run_bg;
            }

          x = end_x;
          co_yield run;
        }
    }
}

} // namespace nxb

