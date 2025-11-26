#pragma once

#include <fmt/color.h>
#include <fmt/core.h>

#include <nix/store/store-api.hh>
#include <nix/util/callback.hh>
#include <nix/util/memory-source-accessor.hh>

#include <boost/stacktrace/stacktrace.hpp>
#include <nix/util/serialise.hh>

namespace nxb {

struct TrivialStoreConfig
    : public std::enable_shared_from_this<TrivialStoreConfig>,
      virtual nix::StoreConfig {
  TrivialStoreConfig(const Params &params) : StoreConfig(params) {}

  static const std::string name() { return "Trivial Store"; }

  static nix::StringSet uriSchemes() { return {"trivial"}; }

  nix::ref<nix::Store> openStore() const override;

  nix::StoreReference getReference() const override {
    return {
        .variant = nix::StoreReference::Specified{.scheme = "trivial"},
        .params = getQueryParams(),
    };
  }
};

struct TrivialStore : virtual nix::Store {
  using Config = TrivialStoreConfig;
  nix::ref<const Config> config;

  TrivialStore(nix::ref<const Config> config)
      : nix::Store{*config}, config(config) {}

  std::string getUri() { return "trivial://"; }

  // === Required pure virtual implementations ===

  void
  queryPathInfoUncached(const nix::StorePath &,
                        nix::Callback<std::shared_ptr<const nix::ValidPathInfo>>
                            callback) noexcept override {
    callback(nullptr); // path not found
  }

  void queryRealisationUncached(
      const nix::DrvOutput &,
      nix::Callback<std::shared_ptr<const nix::Realisation>> callback) noexcept
      override {
    callback(nullptr);
  }

  std::optional<nix::StorePath>
  queryPathFromHashPart(const std::string &) override {
    return std::nullopt;
  }

  void addToStore(const nix::ValidPathInfo &info, nix::Source &,
                  nix::RepairFlag, nix::CheckSigsFlag) override {
    fmt::print("addToStore: path={}\n", printStorePath(info.path));
    unsupported("addToStore");
  }

  nix::StorePath addToStoreFromDump(nix::Source &source, std::string_view name,
                                    nix::FileSerialisationMethod dumpMethod,
                                    nix::ContentAddressMethod hashMethod,
                                    nix::HashAlgorithm hashAlgo,
                                    const nix::StorePathSet &refs,
                                    nix::RepairFlag) override {
    fmt::print("dump {} {} {} {} ({} refs)\n",
               fmt::styled(name, fmt::fg(fmt::color::yellow)),
               dumpMethod == nix::FileSerialisationMethod::NixArchive ? "nar "
                                                                      : "flat",
               hashMethod.render(), printHashAlgo(hashAlgo), refs.size());

    auto counter = nix::LengthSink();
    source.drainInto(counter);
    fmt::print("read {}, {} bytes\n",
               fmt::styled(name, fmt::fg(fmt::color::yellow)), counter.length);

    unsupported("addToStoreFromDump");
  }

  void registerDrvOutput(const nix::Realisation &) override {
    unsupported("registerDrvOutput");
  }

  void narFromPath(const nix::StorePath &, nix::Sink &) override {
    unsupported("narFromPath");
  }

  nix::ref<nix::SourceAccessor> getFSAccessor(bool) override {
    return nix::make_ref<nix::MemorySourceAccessor>();
  }

  std::shared_ptr<nix::SourceAccessor> getFSAccessor(const nix::StorePath &,
                                                     bool) override {
    return nullptr;
  }

  std::optional<nix::TrustedFlag> isTrustedClient() override {
    return nix::Trusted;
  }
};

inline nix::ref<nix::Store> TrivialStoreConfig::openStore() const {
  return nix::make_ref<TrivialStore>(
      nix::ref<const TrivialStoreConfig>(shared_from_this()));
}

// Helper to create one
inline nix::ref<TrivialStore> makeTrivialStore() {
  auto config = nix::make_ref<TrivialStoreConfig>(nix::StoreConfig::Params{});
  return nix::make_ref<TrivialStore>(config);
}

} // namespace nxb
