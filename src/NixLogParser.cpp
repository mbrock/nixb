#include "NixLogParser.hpp"
#include "nix/util/logging.hh"

#include <fmt/color.h>
#include <fmt/core.h>

#include <boost/describe/enum_to_string.hpp>
#include <boost/describe/enumerators.hpp>
#include <boost/mp11/algorithm.hpp>

#include <iterator>

namespace nixb
{

namespace
{
template <typename Enum>
std::optional<Enum>
enum_from_int (int64_t v)
{
  std::optional<Enum> result;
  boost::mp11::mp_for_each<boost::describe::describe_enumerators<Enum>> (
      [&] (auto desc)
        {
          if (!result && static_cast<int64_t> (desc.value) == v)
            {
              result = static_cast<Enum> (desc.value);
            }
        });
  return result;
}

std::string_view
trim_trailing_newline (std::string_view text)
{
  while (!text.empty () && (text.back () == '\n' || text.back () == '\r'))
    {
      text.remove_suffix (1);
    }
  return text;
}
} // namespace

std::string_view
NixLogParser::activity_type_name (ActivityType t)
{
  std::string_view name = boost::describe::enum_to_string (t, "Unknown");
  constexpr std::string_view prefix = "act";
  if (name.rfind (prefix, 0) == 0)
    {
      name.remove_prefix (prefix.size ());
    }
  return name;
}

std::string_view
NixLogParser::result_type_name (ResultType t)
{
  std::string_view name = boost::describe::enum_to_string (t, "UnknownResult");
  constexpr std::string_view prefix = "res";
  if (name.rfind (prefix, 0) == 0)
    {
      name.remove_prefix (prefix.size ());
    }
  return name;
}

std::optional<LogEvent>
NixLogParser::parse_line (std::string_view line)
{
  if (line.rfind ("@nix ", 0) != 0)
    {
      return std::nullopt;
    }

  std::string_view payload{ line.data () + 5, line.size () - 5 };
  auto doc_result = parser_.parse (payload);
  if (doc_result.error ())
    {
      return std::nullopt;
    }

  simdjson::dom::element doc = doc_result.value ();
  auto action_res = doc["action"].get_string ();
  if (action_res.error ())
    {
      return std::nullopt;
    }
  std::string_view action = action_res.value ();

  if (action == "start")
    {
      StartEvent event;
      if (auto v = doc["id"].get_int64 (); !v.error ())
        event.id = v.value ();
      else
        return std::nullopt;
      if (auto v = doc["level"].get_int64 (); !v.error ())
        event.level = v.value ();
      if (auto v = doc["parent"].get_int64 (); !v.error ())
        event.parent = v.value ();
      if (auto v = doc["type"].get_int64 (); !v.error ())
        {
          event.type = enum_from_int<ActivityType> (v.value ())
                           .value_or (nix::actUnknown);
        }
      else
        {
          return std::nullopt;
        }
      if (auto v = doc["text"].get_string (); !v.error ())
        event.text = v.value ();

      if (auto fields_arr = doc["fields"].get_array (); !fields_arr.error ())
        {
          for (auto field : fields_arr.value ())
            {
              if (auto s = field.get_string (); !s.error ())
                {
                  event.fields.emplace_back (s.value ());
                }
            }
          auto parse_store_ref = [&] () -> std::optional<StartEvent::StoreRef>
            {
              if (event.fields.empty ())
                {
                  return std::nullopt;
                }
              StartEvent::StoreRef ref{ event.fields[0], std::nullopt };
              if (event.fields.size () > 1)
                {
                  ref.base_url = event.fields[1];
                }
              return ref;
            };

          switch (event.type)
            {
            case nix::actCopyPath:
            case nix::actQueryPathInfo:
            case nix::actSubstitute:
            case nix::actBuild:
            case nix::actBuildWaiting:
              event.store_ref = parse_store_ref ();
              break;
            default:
              break;
            }

          // Heuristic: some older log lines come through as actUnknown with
          // only a descriptive text. Try to recover type + fields so the UI
          // can treat them like normal transfers.
          auto parse_copying_text = [&] () -> bool
            {
              std::string_view t = event.text;
              constexpr std::string_view prefix = "copying '";
              if (!t.starts_with (prefix))
                return false;

              auto path_start = prefix.size ();
              auto path_end = t.find ('\'', path_start);
              if (path_end == std::string_view::npos)
                return false;
              auto path_sv = t.substr (path_start, path_end - path_start);

              event.type = nix::actCopyPath;
              event.fields.clear ();
              event.fields.emplace_back (std::string (path_sv));
              event.store_ref = parse_store_ref ();
              return true;
            };

          auto parse_hashing_text = [&] () -> bool
            {
              std::string_view t = event.text;
              constexpr std::string_view prefix = "hashing '";
              constexpr char quote = '\'';
              if (!t.starts_with (prefix))
                return false;
              auto path_start = prefix.size ();
              auto path_end = t.find (quote, path_start);
              if (path_end == std::string_view::npos)
                return false;
              auto path_sv = t.substr (path_start, path_end - path_start);
              event.type = nix::actFileTransfer;
              event.fields.clear ();
              event.fields.emplace_back (std::string (path_sv));
              return true;
            };

          if (event.type == nix::actUnknown && event.fields.empty ())
            {
              if (!parse_copying_text ())
                {
                  parse_hashing_text ();
                }
            }
        }

      return event;
    }
  else if (action == "stop")
    {
      StopEvent event;
      if (auto v = doc["id"].get_int64 (); !v.error ())
        event.id = v.value ();
      else
        return std::nullopt;
      return event;
    }
  else if (action == "result")
    {
      ResultEvent event;
      if (auto v = doc["id"].get_int64 (); !v.error ())
        event.id = v.value ();
      else
        return std::nullopt;
      if (auto v = doc["type"].get_int64 (); !v.error ())
        {
          event.type = enum_from_int<ResultType> (v.value ())
                           .value_or (nix::resFileLinked);
        }
      else
        {
          return std::nullopt;
        }

      if (auto fields_arr = doc["fields"].get_array (); !fields_arr.error ())
        {
          for (auto field : fields_arr.value ())
            {
              if (auto s = field.get_string (); !s.error ())
                {
                  event.fields.emplace_back (std::string (s.value ()));
                }
              else if (auto i = field.get_int64 (); !i.error ())
                {
                  event.fields.emplace_back (i.value ());
                }
            }
        }
      return event;
    }
  else if (action == "msg")
    {
      MsgEvent event;
      if (auto v = doc["level"].get_int64 (); !v.error ())
        event.level = v.value ();
      if (auto v = doc["msg"].get_string (); !v.error ())
        event.msg = v.value ();
      return event;
    }

  return std::nullopt;
}

std::string
StartEvent::format () const
{
  fmt::memory_buffer buf;
  fmt::format_to (std::back_inserter (buf), "{}",
                  fmt::styled (">>>", fmt::fg (fmt::terminal_color::blue)));
  // auto id_style = style_for_id (static_cast<uint64_t> (id));
  //  fmt::format_to (
  //      std::back_inserter (buf), " [{} ",
  //      fmt::styled (hashed_id_token (static_cast<uint64_t> (id)),
  //      id_style));
  //  if (parent)
  //    {
  //      fmt::format_to (
  //          std::back_inserter (buf), ":: {}] ",
  //          fmt::styled (hashed_id_token (static_cast<uint64_t> (*parent)),
  //                       style_for_id (static_cast<uint64_t> (*parent))));
  //    }
  //  else
  //    {
  //      fmt::format_to (std::back_inserter (buf), "] ");
  //    }
  if (!text.empty ())
    {
      fmt::format_to (std::back_inserter (buf), "{:<20}: ",
                      fmt::styled (text, fmt::fg (fmt::terminal_color::white)
                                             | fmt::emphasis::bold));
    }
  fmt::format_to (std::back_inserter (buf), "{:<20}",
                  NixLogParser::activity_type_name (type));

  return fmt::to_string (buf);
}

std::string
StopEvent::format (std::string_view type_name, std::string_view activity_text,
                   bool build_success, std::optional<uint64_t> parent_id) const
{
  fmt::memory_buffer buf;
  // auto id_style = style_for_id (static_cast<uint64_t> (id));
  //  std::string parent_token = "-";
  //  if (parent_id)
  //    {
  //      parent_token = fmt::format (
  //          "{}",
  //          fmt::styled (hashed_id_token (static_cast<uint64_t>
  //          (*parent_id)),
  //                       style_for_id (static_cast<uint64_t> (*parent_id))));
  //    }
  fmt::format_to (std::back_inserter (buf), "{} {}",
                  fmt::styled ("<<<", fmt::fg (fmt::terminal_color::red)),
                  // fmt::styled (hashed_id_token (static_cast<uint64_t> (id)),
                  // id_style), parent_token,
                  type_name);
  if (build_success)
    {
      fmt::format_to (
          std::back_inserter (buf), " {}",
          fmt::styled ("OK", fmt::fg (fmt::terminal_color::green)));
    }
  if (!activity_text.empty ())
    {
      fmt::format_to (std::back_inserter (buf), " {}", activity_text);
    }
  return fmt::to_string (buf);
}

std::string
MsgEvent::format () const
{
  return fmt::format (
      "{} {}", fmt::styled ("[msg]", fmt::fg (fmt::terminal_color::cyan)),
      msg);
}

std::optional<std::string_view>
ResultEvent::get_string (size_t idx) const
{
  if (idx >= fields.size ()
      || !std::holds_alternative<std::string> (fields[idx]))
    {
      return std::nullopt;
    }
  return std::get<std::string> (fields[idx]);
}

std::optional<int64_t>
ResultEvent::get_int (size_t idx) const
{
  if (idx >= fields.size () || !std::holds_alternative<int64_t> (fields[idx]))
    {
      return std::nullopt;
    }
  return std::get<int64_t> (fields[idx]);
}

std::string
ResultEvent::format () const
{
  fmt::memory_buffer buf;
  bool printed_compact = false;

  auto print_log_line = [&] (std::string_view msg_view)
    {
      auto faint_style
          = fmt::fg (fmt::terminal_color::white) | fmt::emphasis::faint;
      fmt::format_to (
          std::back_inserter (buf), "{}",
          fmt::styled (fmt::format ("> {}", msg_view), faint_style));
      printed_compact = true;
    };

  if (type == nix::resBuildLogLine || type == nix::resPostBuildLogLine
      || type == nix::resFetchStatus)
    {
      if (auto msg = get_string (0))
        {
          print_log_line (trim_trailing_newline (*msg));
        }
    }
  else if (type == nix::resSetPhase)
    {
      if (auto phase = get_string (0))
        {
          fmt::format_to (
              std::back_inserter (buf), "{} {}",
              fmt::styled ("[phase]", fmt::fg (fmt::terminal_color::magenta)),
              *phase);
          printed_compact = true;
        }
    }

  if (printed_compact)
    {
      return fmt::to_string (buf);
    }

  // Verbose output
  fmt::format_to (
      std::back_inserter (buf), "{} {}",
      fmt::styled ("[result]", fmt::fg (fmt::terminal_color::yellow)),
      NixLogParser::result_type_name (type));

  auto append_str = [&] (const char *label, size_t idx)
    {
      if (auto value = get_string (idx))
        {
          fmt::format_to (std::back_inserter (buf), " {}=\"{}\"", label,
                          *value);
        }
    };

  auto append_int = [&] (const char *label, size_t idx)
    {
      if (auto value = get_int (idx))
        {
          fmt::format_to (std::back_inserter (buf), " {}={}", label, *value);
        }
    };

  switch (type)
    {
    case nix::resBuildLogLine:
    case nix::resPostBuildLogLine:
    case nix::resFetchStatus:
      fmt::format_to (std::back_inserter (buf), " ");
      append_str ("msg", 0);
      break;
    case nix::resSetPhase:
      fmt::format_to (std::back_inserter (buf), " ");
      append_str ("phase", 0);
      break;
    case nix::resProgress:
      fmt::format_to (std::back_inserter (buf), " ");
      append_int ("done", 0);
      append_int ("expected", 1);
      append_int ("running", 2);
      append_int ("failed", 3);
      break;
    case nix::resSetExpected:
      append_int ("activity_type", 0);
      append_int ("expected", 1);
      break;
    case nix::resFileLinked:
      append_int ("done", 0);
      append_int ("total", 1);
      break;
    case nix::resUntrustedPath:
    case nix::resCorruptedPath:
      fmt::format_to (std::back_inserter (buf), " ");
      append_str ("path", 0);
      break;
    default:
      break;
    }

  return fmt::to_string (buf);
}

} // namespace nixb
