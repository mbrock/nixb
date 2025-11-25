#include "raster-diff.hpp"

#include <algorithm>

namespace nxb
{

namespace detail
{

std::optional<std::size_t>
find_next_diff_in_line (const DiffState &state, const std::size_t y,
                        const std::size_t start_x) noexcept
{
  if (y >= state.back->rows () || start_x >= state.back->cols ())
    return std::nullopt;

  // Get 2D views for natural row access
  const auto front_glyphs = state.front->glyphs_2d ();
  const auto back_glyphs = state.back->glyphs_2d ();
  const auto front_fgs = state.front->fgs_2d ();
  const auto back_fgs = state.back->fgs_2d ();
  const auto front_bgs = state.front->bgs_2d ();
  const auto back_bgs = state.back->bgs_2d ();

  // Scan row from start_x to end
  for (std::size_t x = start_x; x < state.back->cols (); ++x)
    {
      if (front_glyphs[y, x] != back_glyphs[y, x]
          || front_fgs[y, x] != back_fgs[y, x]
          || front_bgs[y, x] != back_bgs[y, x])
        {
          return x;
        }
    }

  return std::nullopt;
}

std::size_t
find_run_end (const DiffState &state, const std::size_t y,
              const std::size_t start_x, const std::optional<Rgba8> run_fg,
              const std::optional<Rgba8> run_bg) noexcept
{
  if (start_x + 1 >= state.back->cols ())
    return start_x + 1;

  const auto back_fgs = state.back->fgs_2d ();
  const auto back_bgs = state.back->bgs_2d ();
  constexpr auto default_color = Raster::DEFAULT_COLOR;

  std::size_t end_x = state.back->cols (); // Default to end of line

  // Find where foreground color changes
  if (run_fg.has_value () && *run_fg != default_color)
    {
      // Find first non-matching fg color
      for (std::size_t x = start_x + 1; x < state.back->cols (); ++x)
        {
          if (back_fgs[y, x] != *run_fg)
            {
              end_x = std::min (end_x, x);
              break;
            }
        }
    }
  else if (!run_fg.has_value () || *run_fg == default_color)
    {
      // Run has default fg - find where it becomes non-default
      for (std::size_t x = start_x + 1; x < state.back->cols (); ++x)
        {
          if (back_fgs[y, x] != default_color)
            {
              end_x = std::min (end_x, x);
              break;
            }
        }
    }

  // Find where background color changes
  if (run_bg.has_value () && *run_bg != default_color)
    {
      for (std::size_t x = start_x + 1; x < state.back->cols (); ++x)
        {
          if (back_bgs[y, x] != *run_bg)
            {
              end_x = std::min (end_x, x);
              break;
            }
        }
    }
  else if (!run_bg.has_value () || *run_bg == default_color)
    {
      for (std::size_t x = start_x + 1; x < state.back->cols (); ++x)
        {
          if (back_bgs[y, x] != default_color)
            {
              end_x = std::min (end_x, x);
              break;
            }
        }
    }

  return end_x;
}

} // namespace detail

coro::generator<ChangeRun>
diff_rasters (const Raster &front, const Raster &back)
{
  detail::DiffState state{ &front, &back, std::nullopt, std::nullopt };

  // Get 2D views for color access
  const auto back_fgs = back.fgs_2d ();
  const auto back_bgs = back.bgs_2d ();

  for (std::size_t y = 0; y < back.rows (); ++y)
    {
      std::size_t x = 0;

      while (x < back.cols ())
        {
          // Find next difference in this line
          auto diff_x = detail::find_next_diff_in_line (state, y, x);
          if (!diff_x)
            break; // No more diffs in this line

          x = *diff_x;

          // Determine run colors using mdspan
          const auto run_fg = back_fgs[y, x];
          const auto run_bg = back_bgs[y, x];

          // Find end of color-consistent run
          const std::size_t end_x
              = detail::find_run_end (state, y, x, run_fg, run_bg);

          // Build ChangeRun
          ChangeRun run;
          run.x = x;
          run.y = y;

          // Use linear index for span (still efficient)
          const std::size_t idx = y * back.cols () + x;
          run.glyphs = back.glyphs ().subspan (idx, end_x - x);

          // Determine color changes needed
          constexpr auto default_color = Raster::DEFAULT_COLOR;

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
