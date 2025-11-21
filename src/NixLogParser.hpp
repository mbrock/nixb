#pragma once

#include "IdColor.hpp"

#include <cstdint>
#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <boost/describe/enum.hpp>
#include <nix/util/logging.hh>

BOOST_DESCRIBE_ENUM(nix::ActivityType, actUnknown, actCopyPath, actFileTransfer,
                    actRealise, actCopyPaths, actBuilds, actBuild,
                    actOptimiseStore, actVerifyPaths, actSubstitute,
                    actQueryPathInfo, actPostBuildHook, actBuildWaiting,
                    actFetchTree)

BOOST_DESCRIBE_ENUM(nix::ResultType, resFileLinked, resBuildLogLine,
                    resUntrustedPath, resCorruptedPath, resSetPhase,
                    resProgress, resSetExpected, resPostBuildLogLine,
                    resFetchStatus)

namespace nixb {

using ActivityType = nix::ActivityType;
using ResultType = nix::ResultType;
using ActivityId = nix::ActivityId;

struct StartEvent {
  int64_t id;
  int64_t level;
  ActivityType type;
  std::string text;
  std::vector<std::string> fields;
  std::optional<int64_t> parent;

  std::string format() const;
};

struct StopEvent {
  int64_t id;

  std::string format(std::string_view type_name, std::string_view activity_text,
                     bool build_success, std::optional<uint64_t> parent_id) const;
};

struct ResultEvent {
  int64_t id;
  ResultType type;
  std::vector<std::variant<int64_t, std::string>> fields;

  std::optional<std::string_view> get_string(size_t idx) const;
  std::optional<int64_t> get_int(size_t idx) const;
  std::string format() const;
};

struct MsgEvent {
  int64_t level;
  std::string msg;

  std::string format() const;
};

using LogEvent = std::variant<StartEvent, StopEvent, ResultEvent, MsgEvent>;

class NixLogParser {
public:
  NixLogParser() = default;

  // Parse a single line of JSON log.
  // Returns a LogEvent if successful, or std::nullopt if the line is not a
  // valid @nix log line or parsing fails.
  std::optional<LogEvent> parse_line(std::string_view line);

  static std::string_view activity_type_name(ActivityType t);
  static std::string_view result_type_name(ResultType t);

private:
  simdjson::dom::parser parser_;
};

} // namespace nixb
