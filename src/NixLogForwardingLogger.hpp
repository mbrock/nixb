#pragma once

#include <functional>
#include <mutex>
#include <string>

#include <nix/util/logging.hh>

namespace nixb
{

/**
 * A Logger that serializes log events to the same JSON shape as
 * `--log-format internal-json` and forwards each line to a sink
 * function. This lets us feed an in-process NixLogWatcher without
 * going through stderr/stdout.
 */
class NixLogForwardingLogger : public nix::Logger
{
public:
  using Sink = std::function<void (const std::string &)>;

  explicit NixLogForwardingLogger (Sink sink, bool include_prefix = true,
                                   std::atomic<bool> *stop_flag = nullptr);

  void log (nix::Verbosity lvl, std::string_view s) override;
  void logEI (const nix::ErrorInfo &ei) override;
  void startActivity (nix::ActivityId act, nix::Verbosity lvl,
                      nix::ActivityType type, const std::string &text,
                      const nix::Logger::Fields &fields,
                      nix::ActivityId parent) override;
  void stopActivity (nix::ActivityId act) override;
  void result (nix::ActivityId act, nix::ResultType type,
               const nix::Logger::Fields &fields) override;

private:
  void emit (nlohmann::json &&json);
  static void add_fields (nlohmann::json &json,
                          const nix::Logger::Fields &fields);

  Sink sink_;
  bool include_prefix_;
  std::atomic<bool> *stop_flag_ = nullptr;
  std::mutex mutex_;
};

} // namespace nixb
