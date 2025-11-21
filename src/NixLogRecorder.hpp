#pragma once

#include <chrono>
#include <fstream>
#include <string>

namespace nixb
{

class NixLogRecorder
{
public:
  explicit NixLogRecorder (const std::string &path);

  bool
  enabled () const
  {
    return enabled_;
  }
  void record (const std::string &line);

private:
  bool enabled_ = false;
  std::ofstream stream_;
  std::chrono::steady_clock::time_point start_time_;
  bool start_time_set_ = false;
};

} // namespace nixb
