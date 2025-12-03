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

// Extract the key error message (first error: line)
std::string extract_key_error(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.find("error:") != std::string::npos) {
            // Truncate long lines
            if (line.size() > 100) {
                return line.substr(0, 100) + "...";
            }
            return line;
        }
    }
    return "build failed";
}

void output_markdown(const BuildSummary& summary, std::ostream& out, bool use_details = true) {
    out << "# Nix Build Log\n\n";

    // Summary
    bool success = summary.failed_drvs.empty() && summary.errors.empty();
    out << "## Summary\n\n";
    out << "- **Status:** " << (success ? "✅ Success" : "❌ Failed") << "\n";
    if (summary.total_expected > 0) {
        out << "- **Progress:** " << summary.total_done << "/" << summary.total_expected << " derivations\n";
    }
    if (!summary.substituted.empty()) {
        out << "- **Substituted:** " << summary.substituted.size() << " paths\n";
    }
    if (!summary.failed_drvs.empty()) {
        out << "- **Failed:** " << summary.failed_drvs.size() << " derivations\n";
    }
    out << "\n";

    // Failed derivations with details
    if (!summary.failed_drvs.empty()) {
        out << "## Failed Builds\n\n";
    }

    // Build logs for failed activities
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
                auto key_error = extract_key_error(act.log_lines);

                if (use_details) {
                    out << "<details>\n";
                    out << "<summary><code>" << name << "</code>: " << key_error << "</summary>\n\n";
                } else {
                    out << "### " << name << "\n\n";
                    out << "**Error:** " << key_error << "\n\n";
                }

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

                if (use_details) {
                    out << "</details>\n\n";
                }
            }
        }
    }

    // Top-level errors (like "Cannot build" messages)
    bool has_toplevel_errors = false;
    for (const auto& err : summary.errors) {
        if (err.find("Last ") != std::string::npos && err.find("log lines") != std::string::npos) {
            has_toplevel_errors = true;
            break;
        }
    }

    if (has_toplevel_errors) {
        if (use_details) {
            out << "<details>\n";
            out << "<summary><strong>Full Error Messages</strong></summary>\n\n";
        } else {
            out << "## Full Error Messages\n\n";
        }

        for (const auto& err : summary.errors) {
            if (err.find("Last ") != std::string::npos && err.find("log lines") != std::string::npos) {
                out << "```\n" << err << "\n```\n\n";
            }
        }

        if (use_details) {
            out << "</details>\n\n";
        }
    }

    // Warnings
    if (!summary.warnings.empty()) {
        if (use_details) {
            out << "<details>\n";
            out << "<summary><strong>Warnings (" << summary.warnings.size() << ")</strong></summary>\n\n";
        } else {
            out << "## Warnings\n\n";
        }

        for (const auto& warn : summary.warnings) {
            out << "- " << warn << "\n";
        }
        out << "\n";

        if (use_details) {
            out << "</details>\n\n";
        }
    }
}

void process_file(const std::string& path, BuildSummary& summary) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Warning: Cannot open " << path << "\n";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            process_line(line, summary);
        }
    }
}

// Process a single file and output as a section
void process_file_standalone(const std::string& path, std::ostream& out) {
    BuildSummary summary;

    std::ifstream file(path);
    if (!file) {
        std::cerr << "Warning: Cannot open " << path << "\n";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            process_line(line, summary);
        }
    }

    // Extract gem name from filename
    auto basename = path.substr(path.find_last_of("/\\") + 1);
    auto ext_pos = basename.find(".fail.log");
    if (ext_pos == std::string::npos) ext_pos = basename.find(".ok.log");
    if (ext_pos == std::string::npos) ext_pos = basename.find(".log");
    std::string gem_name = (ext_pos != std::string::npos) ? basename.substr(0, ext_pos) : basename;

    bool success = summary.failed_drvs.empty() && summary.errors.empty();
    std::string status_icon = success ? "✅" : "❌";

    // Extract key error
    std::string key_error = success ? "build succeeded" : "build failed";
    for (const auto& [id, act] : summary.activities) {
        if (act.type == ActivityType::Build && !act.log_lines.empty()) {
            key_error = extract_key_error(act.log_lines);
            break;
        }
    }

    out << "<details>\n";
    out << "<summary>" << status_icon << " <strong>" << gem_name << "</strong>: " << key_error << "</summary>\n\n";

    // Build logs
    for (const auto& [id, act] : summary.activities) {
        if (act.type == ActivityType::Build && !act.log_lines.empty()) {
            if (!act.phase.empty()) {
                out << "Phase: `" << act.phase << "`\n\n";
            }
            out << "```\n";
            size_t start = act.log_lines.size() > 80 ? act.log_lines.size() - 80 : 0;
            for (size_t i = start; i < act.log_lines.size(); ++i) {
                out << act.log_lines[i] << "\n";
            }
            out << "```\n\n";
        }
    }

    out << "</details>\n\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        // Read from stdin
        BuildSummary summary;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty()) {
                process_line(line, summary);
            }
        }
        output_markdown(summary, std::cout);
        return 0;
    }

    // Check for --multi flag
    bool multi_mode = false;
    std::vector<std::string> files;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--multi" || arg == "-m") {
            multi_mode = true;
        } else {
            files.push_back(arg);
        }
    }

    if (multi_mode && files.size() > 1) {
        // Multi-file mode: each file becomes a details section
        std::cout << "# Ruby Gem Build Results (Fil-C)\n\n";

        // Count successes and failures
        int ok = 0, fail = 0;
        for (const auto& f : files) {
            if (f.find(".ok.log") != std::string::npos) ok++;
            else if (f.find(".fail.log") != std::string::npos) fail++;
        }

        std::cout << "**" << ok << " succeeded**, **" << fail << " failed**\n\n";
        std::cout << "---\n\n";

        for (const auto& f : files) {
            process_file_standalone(f, std::cout);
        }
    } else if (files.size() == 1) {
        // Single file mode
        BuildSummary summary;
        process_file(files[0], summary);
        output_markdown(summary, std::cout);
    } else {
        std::cerr << "Usage: nix-log-to-md [--multi] <file.log> [file2.log ...]\n";
        std::cerr << "       nix-log-to-md < build.log\n";
        return 1;
    }

    return 0;
}
