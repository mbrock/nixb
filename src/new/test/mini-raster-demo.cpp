#include "../glyph-table.hpp"
#include "../tty-raster-diff.hpp"
#include "../tty-raster.hpp"

#include <array>
#include <fmt/core.h>
#include <string>

namespace nxb::test
{

struct Step
{
  const char *label;
  void (*apply) (Raster &raster, GlyphTable &glyphs);
};

namespace
{

char
glyph_to_char (const GlyphTable::GlyphId gid, const GlyphTable &glyphs)
{
  if (const auto view = glyphs.get (gid))
    {
      if (const auto str = *view; !str.empty ())
        return str.front ();
    }
  return '?';
}

std::string
glyph_span_to_string (const std::span<const GlyphTable::GlyphId> ids,
                      GlyphTable &glyphs)
{
  std::string result;
  result.reserve (ids.size ());
  for (const auto gid : ids)
    result.push_back (glyph_to_char (gid, glyphs));
  return result;
}

void
dump_snapshot (const Raster &raster, GlyphTable &glyphs)
{
  fmt::print ("Snapshot:\n");
  for (std::size_t y = 0; y < raster.height (); ++y)
    {
      std::string row;
      row.reserve (raster.width ());
      for (std::size_t x = 0; x < raster.width (); ++x)
        {
          if (const auto cell = raster.get_cell (x, y))
            row.push_back (glyph_to_char (cell->glyph, glyphs));
          else
            row.push_back ('?');
        }
      fmt::print ("  {}\n", row);
    }
}

void
dump_diff (const Raster &front, const Raster &back, GlyphTable &glyphs)
{
  fmt::print ("Diff runs:\n");
  bool any = false;
  for (const auto &run : diff_rasters (front, back))
    {
      any = true;
      fmt::print ("  ({}, {}) -> \"{}\"\n", run.x, run.y,
                  glyph_span_to_string (run.glyphs, glyphs));
    }
  if (!any)
    fmt::print ("  (no changes)\n");
}

} // namespace

int
run_mini_raster_demo ()
{
  GlyphTable glyphs;
  Raster front (4, 4);
  Raster back (4, 4);
  front.clear ();
  back.clear ();

  constexpr std::array<Step, 4> steps{ {
      {
          "Blank grid",
          [] (Raster &r, GlyphTable &) { r.clear (); },
      },
      {
          "Corner markers",
          [] (Raster &r, GlyphTable &) {
            r.clear ();
            r.set_char (0, 0, 'A');
            r.set_char (3, 0, 'B');
            r.set_char (0, 3, 'C');
            r.set_char (3, 3, 'D');
          },
      },
      {
          "Horizontal bar",
          [] (Raster &r, GlyphTable &g) {
            r.clear ();
            const auto gid = g.intern ("=");
            r.fill_rect (0, 1, r.width (), 1, gid, Rgba8 (fmt::color::green));
          },
      },
      {
          "Diagonal line",
          [] (Raster &r, GlyphTable &) {
            r.clear ();
            for (std::size_t i = 0; i < std::min (r.width (), r.height ());
                 ++i)
              r.set_char (i, i, '#');
          },
      },
  } };

  for (const auto &[label, apply] : steps)
    {
      fmt::print ("== {} ==\n", label);
      back.clear ();
      apply (back, glyphs);
      dump_snapshot (back, glyphs);
      dump_diff (front, back, glyphs);
      front = back;
      fmt::print ("\n");
    }

  fmt::print ("Mini raster demo complete.\n");
  return 0;
}

} // namespace nxb::test

int
main ()
{
  return nxb::test::run_mini_raster_demo ();
}
