#pragma once

#include <cstdint>
#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <boost/describe/enum.hpp>

namespace nixb {

enum class ActivityType {
  Unknown = 0,
  CopyPath = 100,
  FileTransfer = 101,
  Realise = 102,
  CopyPaths = 103,
  Builds = 104,
  Build = 105,
  OptimiseStore = 106,
  VerifyPaths = 107,
  Substitute = 108,
  QueryPathInfo = 109,
  PostBuildHook = 110,
  BuildWaiting = 111,
  FetchTree = 112,
};

BOOST_DESCRIBE_ENUM(ActivityType, Unknown, CopyPath, FileTransfer, Realise,
                    CopyPaths, Builds, Build, OptimiseStore, VerifyPaths,
                    Substitute, QueryPathInfo, PostBuildHook, BuildWaiting,
                    FetchTree)

enum class ResultType {
  FileLinked = 100,
  BuildLogLine = 101,
  UntrustedPath = 102,
  CorruptedPath = 103,
  SetPhase = 104,
  Progress = 105,
  SetExpected = 106,
  PostBuildLogLine = 107,
  FetchStatus = 108,
};

BOOST_DESCRIBE_ENUM(ResultType, FileLinked, BuildLogLine, UntrustedPath,
                    CorruptedPath, SetPhase, Progress, SetExpected,
                    PostBuildLogLine, FetchStatus)

struct StartEvent {
  int64_t id;
  int64_t level;
  ActivityType type;
  std::string text;
  std::vector<std::string> fields;
};

struct StopEvent {
  int64_t id;
};

struct ResultEvent {
  int64_t id;
  ResultType type;
  std::vector<std::variant<int64_t, std::string>> fields;
};

struct MsgEvent {
  int64_t level;
  std::string msg;
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
