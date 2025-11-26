#include "nxt/glyph-table.hpp"

#include <stdexcept>

namespace nxb
{

  GlyphTable::GlyphTable ()
  {
    // Pre-populate ASCII 0-255
    arena_.reserve (256);
    spans_.reserve (256);
    table_.reserve (256);

    for (std::uint32_t i = 0; i <= ASCII_MAX; ++i)
      {
        const auto offset = static_cast<std::uint32_t> (arena_.size ());
        arena_.push_back (static_cast<char> (i));
        spans_.push_back ({ offset, 1 });

        // Create string_view into arena for hash map key
        std::string_view key{ &arena_[offset], 1 };
        table_.emplace (key, static_cast<GlyphId> (i));
      }
  }

  GlyphTable::GlyphId
  GlyphTable::intern (const std::string_view bytes)
  {
    // Fast path for single bytes (no lock needed - ASCII is immutable)
    if (bytes.size () == 1)
      return static_cast<unsigned char> (bytes[0]);

    std::lock_guard<std::mutex> lock (mutex_);

    // Check if already interned
    if (const auto it = table_.find (bytes); it != table_.end ())
      return it->second;

    // Intern new glyph
    if (bytes.size () > 255)
      throw std::length_error ("Glyph too long (max 255 bytes)");

    const auto offset = static_cast<std::uint32_t> (arena_.size ());
    const auto length = static_cast<std::uint8_t> (bytes.size ());

    // Append to arena
    arena_.insert (arena_.end (), bytes.begin (), bytes.end ());

    // Track span
    spans_.push_back ({ offset, length });
    auto id = static_cast<GlyphId> (spans_.size () - 1);

    // Create stable string_view into arena for hash map
    std::string_view key{ arena_.data () + offset, length };
    table_.emplace (key, id);

    return id;
  }

  std::optional<std::span<const char>>
  GlyphTable::get_span (const GlyphId id) const noexcept
  {
    if (id >= spans_.size ())
      return std::nullopt;

    const auto &[offset, length] = spans_[id];
    return std::span{ arena_.data () + offset, length };
  }

  std::optional<std::string_view>
  GlyphTable::get (const GlyphId id) const noexcept
  {
    if (const auto span = get_span (id))
      return std::string_view{ span->data (), span->size () };
    return std::nullopt;
  }

  std::string_view
  GlyphTable::operator[] (const GlyphId id) const
  {
    if (const auto sv = get (id))
      return *sv;
    throw std::out_of_range ("Invalid GlyphId");
  }

  std::size_t
  GlyphTable::size () const noexcept
  {
    return spans_.size ();
  }

  void
  GlyphTable::clear ()
  {
    table_.clear ();
    arena_.clear ();
    spans_.clear ();
  }

} // namespace nxb
