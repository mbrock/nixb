#include "NixLogForwardingLogger.hpp"

#include <nix/util/error.hh>
#include <nix/util/logging.hh>
#include <nix/util/position.hh>
#include <nix/util/signals.hh>
#include <nlohmann/json.hpp>
#include <sstream>

namespace nix
{
/* Forward decl for the libutil-defined serializer so we can call it. */
void to_json (nlohmann::json &json, std::shared_ptr<const Pos> pos);
}

namespace nixb
{

namespace
{
std::string
make_line (const nlohmann::json &json, bool include_prefix)
{
  return (include_prefix ? "@nix " : std::string{})
         + json.dump (-1, ' ', false,
                      nlohmann::json::error_handler_t::replace);
}
} // namespace

NixLogForwardingLogger::NixLogForwardingLogger (Sink sink, bool include_prefix,
                                                std::atomic<bool> *stop_flag)
    : sink_ (std::move (sink)), include_prefix_ (include_prefix),
      stop_flag_ (stop_flag)
{
}

void
NixLogForwardingLogger::emit (nlohmann::json &&json)
{
  std::lock_guard<std::mutex> lock (mutex_);
  if (stop_flag_ && stop_flag_->load (std::memory_order_relaxed))
    throw nix::Interrupted ("interrupted");
  sink_ (make_line (json, include_prefix_));
}

void
NixLogForwardingLogger::add_fields (nlohmann::json &json,
                                    const nix::Logger::Fields &fields)
{
  nlohmann::json fields_json = nlohmann::json::array ();
  for (auto &field : fields)
    {
      if (field.type == nix::Logger::Field::tString)
        fields_json.push_back (field.s);
      else
        fields_json.push_back (field.i);
    }
  json["fields"] = std::move (fields_json);
}

void
NixLogForwardingLogger::log (nix::Verbosity lvl, std::string_view s)
{
  nlohmann::json json;
  json["action"] = "msg";
  json["level"] = lvl;
  json["msg"] = s;
  emit (std::move (json));
}

void
NixLogForwardingLogger::logEI (const nix::ErrorInfo &ei)
{
  std::ostringstream oss;
  nix::showErrorInfo (oss, ei, nix::loggerSettings.showTrace.get ());

  nlohmann::json json;
  json["action"] = "msg";
  json["level"] = ei.level;
  json["msg"] = oss.str ();
  json["raw_msg"] = ei.msg.str ();
  nix::to_json (json, ei.pos);

  if (nix::loggerSettings.showTrace.get () && !ei.traces.empty ())
    {
      nlohmann::json traces = nlohmann::json::array ();
      for (auto iter = ei.traces.rbegin (); iter != ei.traces.rend (); ++iter)
        {
          nlohmann::json frame;
          frame["raw_msg"] = iter->hint.str ();
          nix::to_json (frame, iter->pos);
          traces.push_back (std::move (frame));
        }
      json["trace"] = std::move (traces);
    }

  emit (std::move (json));
}

void
NixLogForwardingLogger::startActivity (nix::ActivityId act, nix::Verbosity lvl,
                                       nix::ActivityType type,
                                       const std::string &text,
                                       const nix::Logger::Fields &fields,
                                       nix::ActivityId parent)
{
  nlohmann::json json;
  json["action"] = "start";
  json["id"] = act;
  json["level"] = lvl;
  json["type"] = type;
  json["text"] = text;
  json["parent"] = parent;
  add_fields (json, fields);
  emit (std::move (json));
}

void
NixLogForwardingLogger::stopActivity (nix::ActivityId act)
{
  nlohmann::json json;
  json["action"] = "stop";
  json["id"] = act;
  emit (std::move (json));
}

void
NixLogForwardingLogger::result (nix::ActivityId act, nix::ResultType type,
                                const nix::Logger::Fields &fields)
{
  nlohmann::json json;
  json["action"] = "result";
  json["id"] = act;
  json["type"] = type;
  add_fields (json, fields);
  emit (std::move (json));
}

} // namespace nixb
