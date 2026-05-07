#include "NixLogRecorder.hpp"

#include <fmt/core.h>

namespace nixb
{

NixLogRecorder::NixLogRecorder (const std::string &path)
{
  stream_.open (path, std::ios::out | std::ios::trunc);
  if (stream_)
    {
      enabled_ = true;
    }
  else
    {
      fmt::print (stderr, "Failed to open record file: {}\n", path);
    }
}

void
NixLogRecorder::record (const std::string &line)
{
  if (!enabled_ || !stream_)
    {
      return;
    }

  if (!start_time_set_)
    {
      start_time_ = std::chrono::steady_clock::now ();
      start_time_set_ = true;
    }

  auto now = std::chrono::steady_clock::now ();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                        now - start_time_)
                        .count ();
  stream_ << fmt::format ("{:013d} {}\n", elapsed_ms, line);
}

} // namespace nixb
