#include "nix_log_parser.hpp"

#include <boost/describe/enum_to_string.hpp>
#include <boost/describe/enumerators.hpp>
#include <boost/mp11/algorithm.hpp>

namespace nixb {

namespace {
template <typename Enum> std::optional<Enum> enum_from_int(int64_t v) {
  std::optional<Enum> result;
  boost::mp11::mp_for_each<boost::describe::describe_enumerators<Enum>>(
      [&](auto desc) {
        if (!result && static_cast<int64_t>(desc.value) == v) {
          result = static_cast<Enum>(desc.value);
        }
      });
  return result;
}
} // namespace

std::string_view NixLogParser::activity_type_name(ActivityType t) {
  return boost::describe::enum_to_string(t, "Unknown");
}

std::string_view NixLogParser::result_type_name(ResultType t) {
  return boost::describe::enum_to_string(t, "UnknownResult");
}

std::optional<LogEvent> NixLogParser::parse_line(std::string_view line) {
  if (line.rfind("@nix ", 0) != 0) {
    return std::nullopt;
  }

  std::string_view payload{line.data() + 5, line.size() - 5};
  auto doc_result = parser_.parse(payload);
  if (doc_result.error()) {
    return std::nullopt;
  }

  simdjson::dom::element doc = doc_result.value();
  auto action_res = doc["action"].get_string();
  if (action_res.error()) {
    return std::nullopt;
  }
  std::string_view action = action_res.value();

  if (action == "start") {
    StartEvent event;
    if (auto v = doc["id"].get_int64(); !v.error())
      event.id = v.value();
    else
      return std::nullopt;
    if (auto v = doc["level"].get_int64(); !v.error())
      event.level = v.value();
    if (auto v = doc["type"].get_int64(); !v.error()) {
      event.type = enum_from_int<ActivityType>(v.value()).value_or(
          ActivityType::Unknown);
    } else {
      return std::nullopt;
    }
    if (auto v = doc["text"].get_string(); !v.error())
      event.text = v.value();

    if (auto fields_arr = doc["fields"].get_array(); !fields_arr.error()) {
      for (auto field : fields_arr.value()) {
        if (auto s = field.get_string(); !s.error()) {
          event.fields.emplace_back(s.value());
        }
      }
    }
    return event;
  } else if (action == "stop") {
    StopEvent event;
    if (auto v = doc["id"].get_int64(); !v.error())
      event.id = v.value();
    else
      return std::nullopt;
    return event;
  } else if (action == "result") {
    ResultEvent event;
    if (auto v = doc["id"].get_int64(); !v.error())
      event.id = v.value();
    else
      return std::nullopt;
    if (auto v = doc["type"].get_int64(); !v.error()) {
      event.type =
          enum_from_int<ResultType>(v.value()).value_or(ResultType::FileLinked);
    } else {
      return std::nullopt;
    }

    if (auto fields_arr = doc["fields"].get_array(); !fields_arr.error()) {
      for (auto field : fields_arr.value()) {
        if (auto s = field.get_string(); !s.error()) {
          event.fields.emplace_back(std::string(s.value()));
        } else if (auto i = field.get_int64(); !i.error()) {
          event.fields.emplace_back(i.value());
        }
      }
    }
    return event;
  } else if (action == "msg") {
    MsgEvent event;
    if (auto v = doc["level"].get_int64(); !v.error())
      event.level = v.value();
    if (auto v = doc["msg"].get_string(); !v.error())
      event.msg = v.value();
    return event;
  }

  return std::nullopt;
}

} // namespace nixb
