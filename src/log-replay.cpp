#include "log-replay.hpp"

#include <charconv>
#include <ext/stdio_filebuf.h>
#include <fcntl.h>
#include <fstream>
#include <simdjson.h>
#include <unistd.h>

namespace nixb::replay
{

namespace ev = nix_event;

namespace
{

/// Parse JSON "fields" array into nix::Logger::Fields.
ev::Fields
parse_json_fields (simdjson::ondemand::array &arr)
{
  ev::Fields fields;
  for (auto field : arr)
    {
      auto type = field.type ().value ();
      if (type == simdjson::ondemand::json_type::string)
        fields.emplace_back (std::string (field.get_string ().value ()));
      else if (type == simdjson::ondemand::json_type::number)
        fields.emplace_back (
            static_cast<uint64_t> (field.get_uint64 ().value ()));
    }
  return fields;
}

/// Parse a JSON log entry and return the semantic event.
std::optional<Event>
parse_json_event (simdjson::ondemand::document &doc)
{
  auto action = doc["action"].get_string ();
  if (action.error ())
    return std::nullopt;

  std::string_view action_sv = action.value ();

  if (action_sv == "msg")
    {
      auto level = doc["level"].get_int64 ();
      auto msg = doc["msg"].get_string ();
      if (level.error () || msg.error ())
        return std::nullopt;

      return ev::LogLine{
        .level = static_cast<nix::Verbosity> (level.value ()),
        .text = std::string (msg.value ()),
      };
    }
  else if (action_sv == "start")
    {
      auto id = doc["id"].get_int64 ();
      auto parent = doc["parent"].get_int64 ();
      auto type = doc["type"].get_int64 ();
      auto text = doc["text"].get_string ();

      if (id.error () || parent.error () || type.error () || text.error ())
        return std::nullopt;

      ev::Fields fields;
      auto fields_arr = doc["fields"].get_array ();
      if (!fields_arr.error ())
        fields = parse_json_fields (fields_arr.value ());

      return ev::ActivityStarted{
        .id = ev::ActivityId{ id.value () },
        .parent = ev::ActivityId{ parent.value () },
        .kind = ev::parse_activity_kind (
            static_cast<nix::ActivityType> (type.value ()),
            std::string (text.value ()), fields),
      };
    }
  else if (action_sv == "stop")
    {
      auto id = doc["id"].get_int64 ();
      if (id.error ())
        return std::nullopt;

      return ev::ActivityFinished{
        .id = ev::ActivityId{ id.value () },
      };
    }
  else if (action_sv == "result")
    {
      auto id = doc["id"].get_int64 ();
      auto type = doc["type"].get_int64 ();

      if (id.error () || type.error ())
        return std::nullopt;

      ev::Fields fields;
      auto fields_arr = doc["fields"].get_array ();
      if (!fields_arr.error ())
        fields = parse_json_fields (fields_arr.value ());

      return ev::parse_result (ev::ActivityId{ id.value () },
                               static_cast<nix::ResultType> (type.value ()),
                               fields);
    }

  return std::nullopt;
}

/// Parse a line, return event and optional timestamp.
struct ParsedLine
{
  std::optional<std::chrono::milliseconds> timestamp;
  std::optional<Event> event;
};

ParsedLine
parse_line (std::string_view line, simdjson::ondemand::parser &parser)
{
  ParsedLine result;

  // Check for timestamp prefix: "<ms> @nix <json>"
  auto space_pos = line.find (' ');
  if (space_pos != std::string_view::npos && space_pos > 0
      && std::isdigit (static_cast<unsigned char> (line[0])))
    {
      int64_t ts_ms = 0;
      auto ts_str = line.substr (0, space_pos);
      auto [ptr, ec] = std::from_chars (
          ts_str.data (), ts_str.data () + ts_str.size (), ts_ms);
      if (ec == std::errc{})
        {
          result.timestamp = std::chrono::milliseconds{ ts_ms };
          line = line.substr (space_pos + 1);
        }
    }

  // Find @nix prefix
  auto nix_pos = line.find ("@nix ");
  if (nix_pos == std::string_view::npos)
    return result;

  auto json_part = line.substr (nix_pos + 5);

  // Parse JSON
  simdjson::padded_string padded (json_part);
  auto doc = parser.iterate (padded);
  if (!doc.error ())
    result.event = parse_json_event (doc.value ());

  return result;
}

} // anonymous namespace

coro::task<>
replay_file (coro::io_scheduler &sched, std::istream &input,
             coro::queue<Event> &queue, std::stop_token stop, bool realtime,
             double speed)
{
  simdjson::ondemand::parser parser;
  std::string line;
  std::chrono::milliseconds last_ts{ 0 };
  bool first = true;

  while (!stop.stop_requested ())
    {
      // Wait for fd to be readable before doing blocking getline
      // auto status = co_await sched.poll (fd, coro::poll_op::read,
      //                                    std::chrono::milliseconds{ 100 });
      // if (status == coro::poll_status::timeout)
      //   continue;
      // if (status != coro::poll_status::event)
      //   break;

      if (!std::getline (input, line))
        break;

      if (line.empty ())
        continue;

      auto parsed = parse_line (line, parser);

      if (parsed.timestamp && realtime && !first)
        {
          auto delay = *parsed.timestamp - last_ts;
          if (delay.count () > 0)
            {
              auto scaled = std::chrono::milliseconds{ static_cast<int64_t> (
                  delay.count () / speed) };
              co_await sched.yield_for (scaled);
            }
        }

      if (parsed.timestamp)
        {
          last_ts = *parsed.timestamp;
          first = false;
        }

      if (parsed.event)
        co_await queue.push (std::move (*parsed.event));
    }
}

coro::task<>
replay_file (coro::io_scheduler &sched, const std::string &path,
             coro::queue<Event> &queue, std::stop_token stop, bool realtime,
             double speed)
{
  std::ifstream input (path);

  co_await replay_file (sched, input, queue, std::move (stop), realtime,
                        speed);
}

} // namespace nixb::replay
