#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nixb
{

enum class ProgressUnit
{
  Count, // Plain numbers (default)
  Bytes  // Byte sizes (show as KiB, MiB, GiB, etc.)
};

struct ActivityProgress
{
  int64_t done = 0;
  int64_t expected = 0;
  int64_t running = 0;
  int64_t failed = 0;
  ProgressUnit unit = ProgressUnit::Count;
};

struct UiActivityLine
{
  int64_t id = 0;
  std::string label;
  std::optional<ActivityProgress> progress;
};

struct UiState
{
  std::vector<UiActivityLine> activity_lines;
};

} // namespace nixb

