#include "log-replay.hpp"
#include "nxt/app.hpp"

#include <charconv>
#include <ext/stdio_filebuf.h>
#include <fcntl.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace nixb::replay {

namespace ev = nix_event;
using json = nlohmann::json;

namespace {

    /// Parse JSON "fields" array into nix::Logger::Fields.
    ev::Fields
    parse_json_fields(const json& arr)
    {
        ev::Fields fields;
        for (const auto& field : arr) {
            if (field.is_string())
                fields.emplace_back(field.get<std::string>());
            else if (field.is_number_unsigned())
                fields.emplace_back(field.get<uint64_t>());
            else if (field.is_number_integer())
                fields.emplace_back(static_cast<uint64_t>(field.get<int64_t>()));
        }
        return fields;
    }

    /// Parse a JSON log entry and return the semantic event.
    std::optional<Event>
    parse_json_event(const json& doc)
    {
        if (!doc.contains("action") || !doc["action"].is_string())
            return std::nullopt;

        std::string action = doc["action"].get<std::string>();

        if (action == "msg") {
            if (!doc.contains("level") || !doc.contains("msg"))
                return std::nullopt;

            return ev::LogLine {
                .level = static_cast<nix::Verbosity>(doc["level"].get<int64_t>()),
                .text = doc["msg"].get<std::string>(),
            };
        } else if (action == "start") {
            if (!doc.contains("id") || !doc.contains("parent")
                || !doc.contains("type") || !doc.contains("text"))
                return std::nullopt;

            ev::Fields fields;
            if (doc.contains("fields") && doc["fields"].is_array())
                fields = parse_json_fields(doc["fields"]);

            return ev::ActivityStarted {
                .id = ev::ActivityId { doc["id"].get<int64_t>() },
                .parent = ev::ActivityId { doc["parent"].get<int64_t>() },
                .kind = ev::parse_activity_kind(
                    static_cast<nix::ActivityType>(doc["type"].get<int64_t>()),
                    doc["text"].get<std::string>(),
                    fields),
            };
        } else if (action == "stop") {
            if (!doc.contains("id"))
                return std::nullopt;

            return ev::ActivityFinished {
                .id = ev::ActivityId { doc["id"].get<int64_t>() },
            };
        } else if (action == "result") {
            if (!doc.contains("id") || !doc.contains("type"))
                return std::nullopt;

            ev::Fields fields;
            if (doc.contains("fields") && doc["fields"].is_array())
                fields = parse_json_fields(doc["fields"]);

            return ev::parse_result(
                ev::ActivityId { doc["id"].get<int64_t>() },
                static_cast<nix::ResultType>(doc["type"].get<int64_t>()),
                fields);
        }

        return std::nullopt;
    }

    /// Parse a line, return event and optional timestamp.
    struct ParsedLine {
        std::optional<std::chrono::milliseconds> timestamp;
        std::optional<Event> event;
    };

    ParsedLine
    parse_line(std::string_view line)
    {
        ParsedLine result;

        // Check for timestamp prefix: "<ms> @nix <json>"
        auto space_pos = line.find(' ');
        if (space_pos != std::string_view::npos
            && space_pos > 0
            && std::isdigit(static_cast<unsigned char>(line[0]))) {
            int64_t ts_ms = 0;
            auto ts_str = line.substr(0, space_pos);
            auto [ptr, ec] = std::from_chars(
                ts_str.data(), ts_str.data() + ts_str.size(), ts_ms);
            if (ec == std::errc {}) {
                result.timestamp = std::chrono::milliseconds { ts_ms };
                line = line.substr(space_pos + 1);
            }
        }

        // Find @nix prefix
        auto nix_pos = line.find("@nix ");
        if (nix_pos == std::string_view::npos)
            return result;

        auto json_part = line.substr(nix_pos + 5);

        // Parse JSON
        try {
            auto doc = json::parse(json_part);
            result.event = parse_json_event(doc);
        } catch (const json::exception&) {
            // Parse error, ignore this line
        }

        return result;
    }

} // anonymous namespace

coro::task<>
replay_file(
    nxb::ui::UIRuntime& runtime,
    std::istream& input,
    coro::queue<Event>& queue,
    bool realtime,
    double speed)
{
    std::string line;
    std::chrono::milliseconds last_ts { 0 };
    bool first = true;

    while (!runtime.shutdown_requested()) {
        if (!std::getline(input, line))
            break;

        if (line.empty())
            continue;

        auto parsed = parse_line(line);

        if (parsed.timestamp && realtime && !first) {
            auto delay = *parsed.timestamp - last_ts;
            if (delay.count() > 0) {
                auto scaled = std::chrono::milliseconds {
                    static_cast<int64_t>(delay.count() / speed)
                };
                co_await runtime.sleep(scaled);
            }
        }

        if (parsed.timestamp) {
            last_ts = *parsed.timestamp;
            first = false;
        }

        if (parsed.event)
            co_await queue.push(std::move(*parsed.event));
    }
}

coro::task<>
replay_file(
    nxb::ui::UIRuntime& runtime,
    const std::string& path,
    coro::queue<Event>& queue,
    bool realtime,
    double speed)
{
    std::ifstream input(path);

    co_await replay_file(runtime, input, queue, realtime, speed);
}

} // namespace nixb::replay
