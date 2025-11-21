#include "NixLogPlayer.hpp"

#include <cctype>
#include <chrono>
#include <fstream>
#include <thread>

#include <fmt/core.h>

namespace nixb
{

NixLogPlayer::NixLogPlayer (LineCallback callback,
                            std::atomic<bool> *stop_flag)
    : callback_ (std::move (callback)), stop_flag_ (stop_flag)
{
}

void
NixLogPlayer::play (const std::string &path, double speedup)
{
  std::ifstream in (path);
  if (!in)
    {
      fmt::print (stderr, "Failed to open playback file: {}\n", path);
      return;
    }

  if (speedup < 0.0)
    {
      speedup = 1.0;
    }

  int64_t last_ms = 0;
  bool first = true;
  std::string line;

  while (std::getline (in, line))
    {
      if (stop_requested ())
        break;

      size_t pos = 0;
      while (pos < line.size ()
             && std::isdigit (static_cast<unsigned char> (line[pos])))
        {
          ++pos;
        }

      int64_t timestamp_ms = 0;
      std::string payload = line;

      if (pos > 0)
        {
          try
            {
              timestamp_ms = std::stoll (line.substr (0, pos));
              if (pos < line.size () && line[pos] == ' ')
                {
                  payload = line.substr (pos + 1);
                }
              else
                {
                  payload = line.substr (pos);
                }
            }
          catch (const std::exception &)
            {
              payload = line;
            }
        }

      int64_t delta = first ? timestamp_ms : (timestamp_ms - last_ms);
      if (delta > 0 && speedup > 0.0)
        {
          double adjusted = static_cast<double> (delta) / speedup;
          if (adjusted > 0.0)
            {
              std::this_thread::sleep_for (
                  std::chrono::milliseconds (static_cast<int64_t> (adjusted)));
            }
        }

      last_ms = timestamp_ms;
      first = false;

      callback_ (payload);

      if (stop_requested ())
        break;
    }
}

} // namespace nixb
