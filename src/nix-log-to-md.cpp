// nix-log-to-md: Convert nix internal-json logs to markdown
//
// Usage: nix-log-to-md < build.log > build.md
//        nix-log-to-md build.log
//
// Parses @nix {...} JSON lines and produces a readable markdown summary.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Activity types from nix
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

// Result types
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
    Builds = 109,
};

struct Activity {
    int64_t id = 0;
    int64_t parent = 0;
    ActivityType type = ActivityType::Unknown;
    std::string text;
    std::string drv_path;
    std::string phase;
    std::vector<std::string> log_lines;
    bool finished = false;
    bool failed = false;
};

struct BuildSummary {
    std::map<int64_t, Activity> activities;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::set<std::string> built_drvs;
    std::set<std::string> failed_drvs;
    std::set<std::string> substituted;
    int total_expected = 0;
    int total_done = 0;
};

// Strip ANSI escape codes
std::string strip_ansi(const std::string& s) {
    static std::regex ansi_re("\x1b\\[[0-9;]*m");
    return std::regex_replace(s, ansi_re, "");
}

// Extract drv name from path
std::string drv_name(const std::string& path) {
    // /nix/store/hash-name.drv -> name
    auto pos = path.find('-');
    if (pos == std::string::npos) return path;
    auto name = path.substr(pos + 1);
    if (name.ends_with(".drv")) {
        name = name.substr(0, name.size() - 4);
    }
    return name;
}

void process_line(const std::string& line, BuildSummary& summary) {
    // Find @nix prefix
    auto pos = line.find("@nix ");
    if (pos == std::string::npos) return;

    auto json_str = line.substr(pos + 5);

    json doc;
    try {
        doc = json::parse(json_str);
    } catch (...) {
        return;
    }

    if (!doc.contains("action")) return;
    std::string action = doc["action"];

    if (action == "start") {
        Activity act;
        act.id = doc.value("id", int64_t(0));
        act.parent = doc.value("parent", int64_t(0));
        act.type = static_cast<ActivityType>(doc.value("type", 0));
        act.text = doc.value("text", "");

        if (doc.contains("fields") && doc["fields"].is_array()) {
            auto& fields = doc["fields"];
            if (act.type == ActivityType::Build && fields.size() >= 1) {
                act.drv_path = fields[0].get<std::string>();
            } else if (act.type == ActivityType::Substitute && fields.size() >= 1) {
                summary.substituted.insert(drv_name(fields[0].get<std::string>()));
            }
        }

        summary.activities[act.id] = act;

    } else if (action == "stop") {
        auto id = doc.value("id", int64_t(0));
        if (summary.activities.count(id)) {
            summary.activities[id].finished = true;
        }

    } else if (action == "result") {
        auto id = doc.value("id", int64_t(0));
        auto type = static_cast<ResultType>(doc.value("type", 0));

        if (doc.contains("fields") && doc["fields"].is_array()) {
            auto& fields = doc["fields"];

            if (type == ResultType::BuildLogLine && fields.size() >= 1) {
                auto text = strip_ansi(fields[0].get<std::string>());
                if (summary.activities.count(id)) {
                    summary.activities[id].log_lines.push_back(text);
                }
            } else if (type == ResultType::SetPhase && fields.size() >= 1) {
                if (summary.activities.count(id)) {
                    summary.activities[id].phase = fields[0].get<std::string>();
                }
            } else if (type == ResultType::Progress && fields.size() >= 4) {
                summary.total_done = fields[0].get<int>();
                summary.total_expected = fields[1].get<int>();
            }
        }

    } else if (action == "msg") {
        auto level = doc.value("level", 0);
        auto msg = strip_ansi(doc.value("msg", ""));

        if (level == 0) { // Error
            summary.errors.push_back(msg);
            // Check for failed derivation
            if (msg.find("build of") != std::string::npos &&
                msg.find("failed") != std::string::npos) {
                // Extract drv path
                auto start = msg.find("/nix/store/");
                if (start != std::string::npos) {
                    auto end = msg.find(".drv", start);
                    if (end != std::string::npos) {
                        auto path = msg.substr(start, end - start + 4);
                        summary.failed_drvs.insert(drv_name(path));
                    }
                }
            }
        } else if (level == 1) { // Warning
            summary.warnings.push_back(msg);
        }
    }
}

void output_markdown(const BuildSummary& summary, std::ostream& out) {
    out << "# Nix Build Log\n\n";

    // Summary
    bool success = summary.failed_drvs.empty() && summary.errors.empty();
    out << "## Summary\n\n";
    out << "- **Status:** " << (success ? "Success" : "Failed") << "\n";
    if (summary.total_expected > 0) {
        out << "- **Progress:** " << summary.total_done << "/" << summary.total_expected << " derivations\n";
    }
    if (!summary.substituted.empty()) {
        out << "- **Substituted:** " << summary.substituted.size() << " paths\n";
    }
    out << "\n";

    // Errors
    if (!summary.errors.empty()) {
        out << "## Errors\n\n";
        for (const auto& err : summary.errors) {
            // Check if it contains log lines
            if (err.find("Last ") != std::string::npos && err.find("log lines") != std::string::npos) {
                out << "```\n" << err << "\n```\n\n";
            } else {
                out << "- " << err << "\n";
            }
        }
        out << "\n";
    }

    // Failed derivations
    if (!summary.failed_drvs.empty()) {
        out << "## Failed Derivations\n\n";
        for (const auto& drv : summary.failed_drvs) {
            out << "- `" << drv << "`\n";
        }
        out << "\n";
    }

    // Build logs for failed activities
    out << "## Build Logs\n\n";
    for (const auto& [id, act] : summary.activities) {
        if (act.type == ActivityType::Build && !act.log_lines.empty()) {
            auto name = drv_name(act.drv_path);

            // Only show if there are errors in the log
            bool has_error = false;
            for (const auto& line : act.log_lines) {
                if (line.find("error:") != std::string::npos ||
                    line.find("Error:") != std::string::npos ||
                    line.find(" error ") != std::string::npos) {
                    has_error = true;
                    break;
                }
            }

            if (has_error || summary.failed_drvs.count(name)) {
                out << "### " << name << "\n\n";
                if (!act.phase.empty()) {
                    out << "Phase: `" << act.phase << "`\n\n";
                }
                out << "```\n";
                // Show last 50 lines
                size_t start = act.log_lines.size() > 50 ? act.log_lines.size() - 50 : 0;
                for (size_t i = start; i < act.log_lines.size(); ++i) {
                    out << act.log_lines[i] << "\n";
                }
                out << "```\n\n";
            }
        }
    }

    // Warnings
    if (!summary.warnings.empty()) {
        out << "## Warnings\n\n";
        for (const auto& warn : summary.warnings) {
            out << "- " << warn << "\n";
        }
        out << "\n";
    }
}

int main(int argc, char** argv) {
    BuildSummary summary;

    std::istream* input = &std::cin;
    std::ifstream file;

    if (argc > 1) {
        file.open(argv[1]);
        if (!file) {
            std::cerr << "Error: Cannot open " << argv[1] << "\n";
            return 1;
        }
        input = &file;
    }

    std::string line;
    while (std::getline(*input, line)) {
        if (!line.empty()) {
            process_line(line, summary);
        }
    }

    output_markdown(summary, std::cout);
    return 0;
}
