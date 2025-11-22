#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace nixb
{

class NixLogPlayer
{
public:
  using LineCallback = std::function<void (const std::string &)>;

  explicit NixLogPlayer (LineCallback callback,
                         std::atomic<bool> *stop_flag = nullptr);

  void play (const std::string &path, double speedup = 1.0);

private:
  bool
  stop_requested () const
  {
    return stop_flag_ && stop_flag_->load (std::memory_order_relaxed);
  }

  LineCallback callback_;
  std::atomic<bool> *stop_flag_ = nullptr;
};

} // namespace nixb
