#include <CLI/CLI.hpp>
#include <fmt/color.h>
#include <fmt/core.h>
#include <simdjson.h>

#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
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

struct ActivityInfo {
    ActivityType type;
    std::string text;
};

std::string_view activity_type_name(ActivityType t) {
    switch (t) {
        case ActivityType::CopyPath: return "CopyPath";
        case ActivityType::FileTransfer: return "FileTransfer";
        case ActivityType::Realise: return "Realise";
        case ActivityType::CopyPaths: return "CopyPaths";
        case ActivityType::Builds: return "Builds";
        case ActivityType::Build: return "Build";
        case ActivityType::OptimiseStore: return "OptimiseStore";
        case ActivityType::VerifyPaths: return "VerifyPaths";
        case ActivityType::Substitute: return "Substitute";
        case ActivityType::QueryPathInfo: return "QueryPathInfo";
        case ActivityType::PostBuildHook: return "PostBuildHook";
        case ActivityType::BuildWaiting: return "BuildWaiting";
        case ActivityType::FetchTree: return "FetchTree";
        default: return "Unknown";
    }
}

std::string_view result_type_name(ResultType t) {
    switch (t) {
        case ResultType::FileLinked: return "FileLinked";
        case ResultType::BuildLogLine: return "BuildLogLine";
        case ResultType::UntrustedPath: return "UntrustedPath";
        case ResultType::CorruptedPath: return "CorruptedPath";
        case ResultType::SetPhase: return "SetPhase";
        case ResultType::Progress: return "Progress";
        case ResultType::SetExpected: return "SetExpected";
        case ResultType::PostBuildLogLine: return "PostBuildLogLine";
        case ResultType::FetchStatus: return "FetchStatus";
        default: return "UnknownResult";
    }
}

ActivityType to_activity_type(int64_t v) {
    switch (v) {
        case 100: return ActivityType::CopyPath;
        case 101: return ActivityType::FileTransfer;
        case 102: return ActivityType::Realise;
        case 103: return ActivityType::CopyPaths;
        case 104: return ActivityType::Builds;
        case 105: return ActivityType::Build;
        case 106: return ActivityType::OptimiseStore;
        case 107: return ActivityType::VerifyPaths;
        case 108: return ActivityType::Substitute;
        case 109: return ActivityType::QueryPathInfo;
        case 110: return ActivityType::PostBuildHook;
        case 111: return ActivityType::BuildWaiting;
        case 112: return ActivityType::FetchTree;
        default: return ActivityType::Unknown;
    }
}

ResultType to_result_type(int64_t v) {
    switch (v) {
        case 100: return ResultType::FileLinked;
        case 101: return ResultType::BuildLogLine;
        case 102: return ResultType::UntrustedPath;
        case 103: return ResultType::CorruptedPath;
        case 104: return ResultType::SetPhase;
        case 105: return ResultType::Progress;
        case 106: return ResultType::SetExpected;
        case 107: return ResultType::PostBuildLogLine;
        case 108: return ResultType::FetchStatus;
        default: return ResultType::FileLinked; // fallback
    }
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"nixb - minimal nix internal-json watcher"};
    bool quiet = false;
    app.add_flag("-q,--quiet", quiet, "suppress pass-through lines that are not @nix JSON");
    CLI11_PARSE(app, argc, argv);

    std::unordered_map<int64_t, ActivityInfo> activities;
    std::optional<int64_t> builds_activity;
    int64_t success_tokens = 0;

    simdjson::dom::parser parser;
    std::string line;

    while (std::getline(std::cin, line)) {
        if (line.rfind("@nix ", 0) == 0) {
            std::string_view payload{line.data() + 5, line.size() - 5};
            auto doc_result = parser.parse(payload);
            if (doc_result.error()) {
                std::fprintf(stderr, "parse error: %s\n", simdjson::error_message(doc_result.error()));
                continue;
            }

            simdjson::dom::element doc = doc_result.value();

            auto action_result = doc["action"].get_string();
            if (action_result.error()) {
                std::fprintf(stderr, "missing action: %s\n", simdjson::error_message(action_result.error()));
                continue;
            }

            std::string_view action = action_result.value();
            fmt::memory_buffer buf;

            if (action == "start") {
                auto id_res = doc["id"].get_int64();
                if (id_res.error()) continue;
                auto type_res = doc["type"].get_int64();
                if (type_res.error()) continue;
                auto text_res = doc["text"].get_string();
                std::string text = text_res.error() ? "" : std::string(text_res.value());
                ActivityType act_type = to_activity_type(type_res.value());
                activities[id_res.value()] = ActivityInfo{act_type, text};
                if (act_type == ActivityType::Builds) {
                    builds_activity = id_res.value();
                }

                fmt::format_to(std::back_inserter(buf), "{} ",
                               fmt::styled("[start]", fmt::fg(fmt::terminal_color::blue)));
                fmt::format_to(std::back_inserter(buf), "{} ", activity_type_name(act_type));
                if (!text.empty()) {
                    fmt::format_to(std::back_inserter(buf), "{}", fmt::styled(text, fmt::fg(fmt::terminal_color::white)));
                }
                fmt::format_to(std::back_inserter(buf), "\n");
                fmt::print("{}", fmt::to_string(buf));
            } else if (action == "result") {
                auto id_res = doc["id"].get_int64();
                auto type_res = doc["type"].get_int64();
                if (id_res.error() || type_res.error()) continue;
                ResultType rt = to_result_type(type_res.value());
                auto fields_res = doc["fields"].get_array();

                // Special, less noisy rendering for common textual results.
                if (!fields_res.error() && (rt == ResultType::BuildLogLine || rt == ResultType::PostBuildLogLine ||
                                            rt == ResultType::FetchStatus)) {
                    auto msg = fields_res.value().at(0).get_string();
                    if (!msg.error()) {
                        fmt::print("{} {}\n", fmt::styled(">", fmt::fg(fmt::terminal_color::white) | fmt::emphasis::faint), msg.value());
                        goto update_tokens;
                    }
                }

                if (!fields_res.error() && rt == ResultType::SetPhase) {
                    auto phase = fields_res.value().at(0).get_string();
                    if (!phase.error()) {
                        fmt::print("{} {}\n", fmt::styled("[phase]", fmt::fg(fmt::terminal_color::magenta)), phase.value());
                        goto update_tokens;
                    }
                }

                fmt::format_to(std::back_inserter(buf), "{} {}",
                               fmt::styled("[result]", fmt::fg(fmt::terminal_color::yellow)),
                               result_type_name(rt));

                if (!fields_res.error()) {
                    auto fields = fields_res.value();
                    auto print_str = [&](const char* label, size_t idx) {
                        if (auto s = fields.at(idx).get_string(); !s.error()) {
                            fmt::format_to(std::back_inserter(buf), " {}=\"{}\"", label, s.value());
                        }
                    };
                    auto print_int = [&](const char* label, size_t idx) {
                        if (auto v = fields.at(idx).get_int64(); !v.error()) {
                            fmt::format_to(std::back_inserter(buf), " {}={}", label, v.value());
                        }
                    };

                    switch (rt) {
                        case ResultType::BuildLogLine:
                        case ResultType::PostBuildLogLine:
                        case ResultType::FetchStatus:
                            fmt::format_to(std::back_inserter(buf), " ");
                            print_str("msg", 0);
                            break;
                        case ResultType::SetPhase:
                            fmt::format_to(std::back_inserter(buf), " ");
                            print_str("phase", 0);
                            break;
                        case ResultType::Progress:
                            fmt::format_to(std::back_inserter(buf), " ");
                            print_int("done", 0);
                            print_int("expected", 1);
                            print_int("running", 2);
                            print_int("failed", 3);
                            break;
                        case ResultType::SetExpected:
                            print_int("activity_type", 0);
                            print_int("expected", 1);
                            break;
                        case ResultType::FileLinked:
                            print_int("done", 0);
                            print_int("total", 1);
                            break;
                        case ResultType::UntrustedPath:
                        case ResultType::CorruptedPath:
                            fmt::format_to(std::back_inserter(buf), " ");
                            print_str("path", 0);
                            break;
                        default:
                            break;
                    }
                }

            update_tokens:
                // Success token logic: Progress on global Builds activity.
                if (rt == ResultType::Progress && builds_activity && *builds_activity == id_res.value()) {
                    auto fields = doc["fields"].get_array();
                    if (!fields.error()) {
                        auto done_res = fields.value().at(0).get_int64();
                        if (!done_res.error()) {
                            static int64_t last_done = 0;
                            int64_t new_done = done_res.value();
                            if (new_done > last_done) {
                                success_tokens += (new_done - last_done);
                                last_done = new_done;
                            }
                        }
                    }
                }

                fmt::format_to(std::back_inserter(buf), "\n");
                fmt::print("{}", fmt::to_string(buf));
            } else if (action == "stop") {
                auto id_res = doc["id"].get_int64();
                if (id_res.error()) continue;
                std::string_view type_name = "Unknown";
                bool build_success = false;
                if (auto it = activities.find(id_res.value()); it != activities.end()) {
                    type_name = activity_type_name(it->second.type);
                    if (it->second.type == ActivityType::Build && success_tokens > 0) {
                        build_success = true;
                        --success_tokens;
                    }
                    activities.erase(it);
                }
                fmt::format_to(std::back_inserter(buf), "{} {}", fmt::styled("[stop]", fmt::fg(fmt::terminal_color::red)),
                               type_name);
                if (build_success) {
                    fmt::format_to(std::back_inserter(buf), " {}", fmt::styled("OK", fmt::fg(fmt::terminal_color::green)));
                }
                if (auto it = activities.find(id_res.value()); it != activities.end() && !it->second.text.empty()) {
                    fmt::format_to(std::back_inserter(buf), " {}", it->second.text);
                }
                fmt::format_to(std::back_inserter(buf), "\n");
                fmt::print("{}", fmt::to_string(buf));
            } else if (action == "msg") {
                // Pass through but label it.
                auto msg_res = doc["msg"].get_string();
                std::string_view msg = msg_res.error() ? "" : msg_res.value();
                fmt::print("{} {}\n", fmt::styled("[msg]", fmt::fg(fmt::terminal_color::cyan)), msg);
            }
        } else if (!quiet) {
            fmt::print("{}\n", line);
        }
    }

    return 0;
}
