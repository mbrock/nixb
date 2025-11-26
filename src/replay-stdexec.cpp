#include "uring.hpp"

#include <charconv>
#include <exec/repeat_effect_until.hpp>
#include <fmt/core.h>
#include <fstream>
#include <simdjson.h>

namespace sx = stdexec;

namespace
{

// Minimal event types
namespace ev
{

struct ActivityId
{
  int64_t value;
};

namespace activity
{
struct Build
{
  std::string drv_path;
};
struct Download
{
  std::string url;
};
struct Copy
{
  std::string path;
};
struct Substitute
{
  std::string path;
};
struct Unknown
{
  int type;
  std::string text;
};
using Kind = std::variant<Build, Download, Copy, Substitute, Unknown>;
} // namespace activity

// Activity type constants (from nix)
constexpr int actBuild = 105;
constexpr int actFileTransfer = 107;
constexpr int actCopyPath = 111;
constexpr int actSubstitute = 108;

struct ActivityStarted
{
  ActivityId id;
  activity::Kind kind;
};

struct ActivityFinished
{
  ActivityId id;
};

struct LogLine
{
  std::string text;
};

using Event = std::variant<ActivityStarted, ActivityFinished, LogLine>;
using Fields = std::vector<std::variant<uint64_t, std::string>>;

std::string
get_string_field (const Fields &fields, size_t idx)
{
  if (idx >= fields.size ())
    return "";
  auto *s = std::get_if<std::string> (&fields[idx]);
  return s ? *s : "";
}

activity::Kind
parse_activity_kind (int type, const std::string &text, const Fields &fields)
{
  switch (type)
    {
    case actBuild:
      return activity::Build{ .drv_path = get_string_field (fields, 0) };
    case actFileTransfer:
      return activity::Download{ .url = get_string_field (fields, 0) };
    case actCopyPath:
      return activity::Copy{ .path = get_string_field (fields, 0) };
    case actSubstitute:
      return activity::Substitute{ .path = get_string_field (fields, 0) };
    default:
      return activity::Unknown{ .type = type, .text = text };
    }
}

} // namespace ev

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

std::optional<ev::Event>
parse_json_event (simdjson::ondemand::document &doc)
{
  auto action = doc["action"].get_string ();
  if (action.error ())
    return std::nullopt;

  std::string_view action_sv = action.value ();

  if (action_sv == "msg")
    {
      auto msg = doc["msg"].get_string ();
      if (msg.error ())
        return std::nullopt;
      return ev::LogLine{ .text = std::string (msg.value ()) };
    }
  else if (action_sv == "start")
    {
      auto id = doc["id"].get_int64 ();
      auto type = doc["type"].get_int64 ();
      auto text = doc["text"].get_string ();

      if (id.error () || type.error () || text.error ())
        return std::nullopt;

      ev::Fields fields;
      auto fields_arr = doc["fields"].get_array ();
      if (!fields_arr.error ())
        fields = parse_json_fields (fields_arr.value ());

      return ev::ActivityStarted{
        .id = ev::ActivityId{ id.value () },
        .kind = ev::parse_activity_kind (static_cast<int> (type.value ()),
                                         std::string (text.value ()), fields),
      };
    }
  else if (action_sv == "stop")
    {
      auto id = doc["id"].get_int64 ();
      if (id.error ())
        return std::nullopt;
      return ev::ActivityFinished{ .id = ev::ActivityId{ id.value () } };
    }

  return std::nullopt;
}

struct ParsedLine
{
  std::optional<std::chrono::milliseconds> timestamp;
  std::optional<ev::Event> event;
};

ParsedLine
parse_line (std::string_view line, simdjson::ondemand::parser &parser)
{
  ParsedLine result;

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

  auto nix_pos = line.find ("@nix ");
  if (nix_pos == std::string_view::npos)
    return result;

  auto json_part = line.substr (nix_pos + 5);

  simdjson::padded_string padded (json_part);
  auto doc = parser.iterate (padded);
  if (!doc.error ())
    result.event = parse_json_event (doc.value ());

  return result;
}

void
print_event (const ev::Event &event)
{
  std::visit (
      [] (auto &&e)
      {
        using T = std::decay_t<decltype (e)>;
        if constexpr (std::is_same_v<T, ev::LogLine>)
          fmt::print ("{}\n", e.text);
        else if constexpr (std::is_same_v<T, ev::ActivityStarted>)
          std::visit (
              [] (auto &&kind)
              {
                using K = std::decay_t<decltype (kind)>;
                if constexpr (std::is_same_v<K, ev::activity::Build>)
                  fmt::print ("▶ building {}\n", kind.drv_path);
                else if constexpr (std::is_same_v<K, ev::activity::Download>)
                  fmt::print ("↓ downloading {}\n", kind.url);
                else if constexpr (std::is_same_v<K, ev::activity::Copy>)
                  fmt::print ("→ copying {}\n", kind.path);
                else if constexpr (std::is_same_v<K, ev::activity::Substitute>)
                  fmt::print ("⇄ substituting {}\n", kind.path);
              },
              e.kind);
        else if constexpr (std::is_same_v<T, ev::ActivityFinished>)
          fmt::print ("✓ activity {} done\n", e.id.value);
      },
      event);
}

/// State for replay
struct ReplayState
{
  std::ifstream input;
  simdjson::ondemand::parser parser;
  std::chrono::milliseconds last_ts{ 0 };
  std::chrono::milliseconds pending_delay{ 0 };
  bool first_event{ true };
  double speed{ 1.0 };
  std::string line_buffer;
  bool done{ false };

  explicit ReplayState (const std::string &path, double spd = 1.0)
      : input (path), speed (spd)
  {
  }

  // Read next line and compute delay. Returns true if EOF.
  bool
  read_next ()
  {
    pending_delay = std::chrono::milliseconds{ 0 };

    if (!std::getline (input, line_buffer))
      {
        done = true;
        return true;
      }

    if (line_buffer.empty ())
      return false;

    auto parsed = parse_line (line_buffer, parser);

    if (parsed.timestamp && !first_event)
      {
        auto delay = *parsed.timestamp - last_ts;
        if (delay.count () > 0)
          pending_delay = std::chrono::milliseconds{ static_cast<int64_t> (
              delay.count () / speed) };
      }

    if (parsed.timestamp)
      {
        last_ts = *parsed.timestamp;
        first_event = false;
      }

    if (parsed.event)
      print_event (*parsed.event);

    return false;
  }
};

} // namespace

int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      fmt::print (stderr, "Usage: {} <logfile> [speed]\n", argv[0]);
      return 1;
    }

  std::string path = argv[1];
  double speed = argc > 2 ? std::stod (argv[2]) : 1.0;

  nxb::uring::io_uring_runtime runtime;
  auto sched = runtime.scheduler ();

  ReplayState state (path, speed);

  if (!state.input)
    {
      fmt::print (stderr, "Failed to open {}\n", path);
      return 1;
    }

  // Simple loop: read line, delay, repeat until EOF
  auto replay_loop
      = sx::just ()
        | sx::then ([&] { state.read_next (); })
        | sx::let_value (
            [&] ()
            {
              // Always schedule_after, even if 0ms (no-op)
              return exec::schedule_after (sched, state.pending_delay);
            })
        | sx::then ([&] { return state.done; })
        | exec::repeat_effect_until ();

  sx::sync_wait (replay_loop);

  fmt::print ("Replay complete.\n");
  return 0;
}
