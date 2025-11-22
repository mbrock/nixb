#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/color.h>
#include <string>
#include <tuple>

namespace nixb
{

inline std::string
encode_base32 (uint64_t value)
{
  static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
  static constexpr std::size_t encoded_length
      = (sizeof (value) * 8U + 4U) / 5U; // ceil(64 / 5) = 13

  std::string out (encoded_length, alphabet[0]);
  for (std::size_t i = 0; i < encoded_length; ++i)
    {
      out[encoded_length - 1 - i] = alphabet[value & 31U];
      value >>= 5U;
      if (value == 0)
        {
          break; // remaining prefix stays padded
        }
    }
  return out;
}

inline uint64_t
hash_id (uint64_t id)
{
  uint64_t x = id + 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
  x = x ^ (x >> 31U);
  return x == 0 ? 1 : x;
}

inline std::tuple<uint8_t, uint8_t, uint8_t>
oklch_to_srgb (float L, float C, float h_rad)
{
  float a_ = C * std::cos (h_rad);
  float b_ = C * std::sin (h_rad);

  float l_ = L + 0.3963377774f * a_ + 0.2158037573f * b_;
  float m_ = L - 0.1055613458f * a_ - 0.0638541728f * b_;
  float s_ = L - 0.0894841775f * a_ - 1.2914855480f * b_;

  float l = l_ * l_ * l_;
  float m = m_ * m_ * m_;
  float s = s_ * s_ * s_;

  float X = 1.2270138511f * l - 0.5577999807f * m + 0.2812561490f * s;
  float Y = -0.0405801784f * l + 1.1122568696f * m - 0.0716766787f * s;
  float Z = -0.0763812845f * l - 0.4214819784f * m + 1.5861632204f * s;

  float r = 4.0767416621f * X - 3.3077115913f * Y + 0.2309699292f * Z;
  float g = -1.2684380046f * X + 2.6097574011f * Y - 0.3413193965f * Z;
  float b = -0.0041960863f * X - 0.7034186147f * Y + 1.7076147010f * Z;

  auto to_srgb = [] (float c) -> uint8_t {
    c = std::clamp (c, 0.0f, 1.0f);
    float v = c <= 0.0031308f ? c * 12.92f
                              : 1.055f * std::pow (c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t> (
        std::round (std::clamp (v, 0.0f, 1.0f) * 255.0f));
  };

  return { to_srgb (r), to_srgb (g), to_srgb (b) };
}

inline std::tuple<float, float, float>
srgb_to_oklch (uint8_t r8, uint8_t g8, uint8_t b8)
{
  // Convert sRGB 0-255 to linear RGB 0-1
  auto from_srgb = [] (uint8_t v) -> float {
    float c = static_cast<float> (v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow ((c + 0.055f) / 1.055f, 2.4f);
  };

  float r = from_srgb (r8);
  float g = from_srgb (g8);
  float b = from_srgb (b8);

  // Linear RGB to XYZ (D65)
  float X = 0.4124564f * r + 0.3575761f * g + 0.1804375f * b;
  float Y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
  float Z = 0.0193339f * r + 0.1191920f * g + 0.9503041f * b;

  // XYZ to LMS
  float l = 0.8189330101f * X + 0.3618667424f * Y - 0.1288597137f * Z;
  float m = 0.0329845436f * X + 0.9293118715f * Y + 0.0361456387f * Z;
  float s = 0.0482003018f * X + 0.2643662691f * Y + 0.6338517070f * Z;

  // Apply cube root
  float l_ = std::cbrt (std::max (l, 0.0f));
  float m_ = std::cbrt (std::max (m, 0.0f));
  float s_ = std::cbrt (std::max (s, 0.0f));

  // LMS to OKLab
  float L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
  float a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
  float b_ = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;

  // OKLab to OKLCH
  float C = std::sqrt (a * a + b_ * b_);
  float h = std::atan2 (b_, a);

  return { L, C, h };
}

inline fmt::rgb
blend_oklch (fmt::rgb fg, fmt::rgb bg, float alpha)
{
  alpha = std::clamp (alpha, 0.0f, 1.0f);

  // Convert both colors to OKLCH
  auto [L1, C1, h1] = srgb_to_oklch (fg.r, fg.g, fg.b);
  auto [L2, C2, h2] = srgb_to_oklch (bg.r, bg.g, bg.b);

  // Interpolate in OKLCH space
  float L = L1 * alpha + L2 * (1.0f - alpha);

  // For chroma, use a power curve to desaturate faster when blending to
  // background This prevents colors from staying too chromatic as they fade
  float alpha_chroma = alpha * alpha; // Square for faster desaturation
  float C = C1 * alpha_chroma + C2 * (1.0f - alpha_chroma);

  // Handle hue interpolation (shortest path around the circle)
  float h_diff = h2 - h1;
  if (h_diff > static_cast<float> (M_PI))
    {
      h_diff -= 2.0f * static_cast<float> (M_PI);
    }
  else if (h_diff < -static_cast<float> (M_PI))
    {
      h_diff += 2.0f * static_cast<float> (M_PI);
    }
  float h = h1 + h_diff * (1.0f - alpha);

  // Convert back to sRGB
  auto [r, g, b] = oklch_to_srgb (L, C, h);
  return fmt::rgb (r, g, b);
}

inline fmt::rgb
color_for_id (uint64_t id)
{
  uint64_t h = hash_id (id);
  float hue = (static_cast<float> (h % 36000) / 36000.0f) * 2.0f
              * static_cast<float> (M_PI); // 0..2pi
  float L = 0.70f;
  float C = 0.08f;
  auto [r, g, b] = oklch_to_srgb (L, C, hue);
  return fmt::rgb (r, g, b);
}

inline fmt::text_style
style_for_id (uint64_t id)
{
  return fmt::fg (color_for_id (id));
}

inline std::string
hashed_id_token (uint64_t id)
{
  return encode_base32 (hash_id (id));
}

} // namespace nixb
