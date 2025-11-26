#pragma once

// Nix API initialization and utilities
//
// Example usage:
//
//   nxb::NixContext ctx;
//
//   // Show derivation JSON for a flake reference
//   auto drv_json = nxb::show_derivation(ctx, ".#default");
//   for (const auto& json : drv_json) {
//     fmt::print("{}\n", json);
//   }
//
//   // Get dependency graph for a derivation
//   auto drv_path =
//   ctx.store()->parseStorePath("/nix/store/...-foo.drv"); auto deps =
//   nxb::get_derivation_deps(ctx, drv_path); for (const auto& [drv,
//   inputs] : deps) {
//     fmt::print("{} depends on {} derivations\n", drv.name(),
//     inputs.size());
//   }

#include <array>
#include <filesystem>
#include <fmt/base.h>
#include <map>
#include <memory>
#include <nix/cmd/common-eval-args.hh>
#include <nix/util/util.hh>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nix/cmd/installable-flake.hh>
#include <nix/cmd/installables.hh>
#include <nix/expr/eval-gc.hh>
#include <nix/expr/eval-settings.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/search-path.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/flake/flake.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/settings.hh>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/outputs-spec.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/ref.hh>

namespace nxb {

// ============================================================================
// Build phases
// ============================================================================

/// Common Nix build phases (stdenv).
/// These appear in order during a typical build.
constexpr std::array<std::string_view, 10> BUILD_PHASES = {
    "unpackPhase", "patchPhase",       "configurePhase", "buildPhase",
    "checkPhase",  "installPhase",     "fixupPhase",     "installCheckPhase",
    "distPhase",   "postInstallPhase",
};

/// Strip "Phase" suffix from phase name for display.
/// "configurePhase" -> "configure"
inline std::string format_phase_name(std::string_view phase) {
  constexpr std::string_view suffix = "Phase";
  std::string result{phase};
  if (result.size() > suffix.size() &&
      result.compare(result.size() - suffix.size(), suffix.size(), suffix) ==
          0) {
    result.erase(result.size() - suffix.size());
  }
  return result;
}

// ============================================================================
// NixContext - RAII initialization
// ============================================================================

/// RAII wrapper for Nix library initialization.
/// Creates a store connection and lazily initializes an EvalState.
class NixContext {
public:
  NixContext() : eval_settings_(read_only_mode_) {
    // fetch_settings_.allowDirty = true;
    // fetch_settings_.warnDirty = false;

    nix::initGC();
    nix::initLibUtil();
    nix::initLibStore();
    store_ = nix::openStore();
  }

  std::shared_ptr<nix::Store> store_ptr() { return store_; }

  nix::ref<nix::Store> store() { return nix::ref<nix::Store>(store_); }

  nix::ref<nix::EvalState> eval_state() {
    if (!eval_state_) {
      eval_state_ = std::make_shared<nix::EvalState>(
          lookup_path_, store(), fetch_settings_, eval_settings_);

      nix::flakeSettings.configureEvalSettings(eval_settings_);
      eval_settings_.evalCores.override(0);
      eval_settings_.lazyTrees.override(true);
      eval_settings_.traceVerbose.override(true);
      eval_settings_.useEvalCache.override(true);
    }

    return nix::ref<nix::EvalState>(eval_state_);
  }

private:
  std::shared_ptr<nix::Store> store_;
  std::shared_ptr<nix::EvalState> eval_state_;

  nix::LookupPath lookup_path_{};
  bool read_only_mode_ = true;
  nix::fetchers::Settings fetch_settings_;
  nix::EvalSettings eval_settings_;
};

// ============================================================================
// Derivation introspection
// ============================================================================

/// Summary of a derivation for display/tracking purposes.
struct DerivationInfo {
  nix::StorePath path;
  std::string name;
  std::string system;
  std::vector<nix::StorePath> input_drvs;
  std::vector<nix::StorePath> output_paths;
  //  std::map<std::string, std::string, std::less<>> env;
  //  nlohmann::json attrs;

  DerivationInfo(nix::StorePath p) : path(std::move(p)) {}
};

/// Read a derivation and extract its info.
inline std::optional<DerivationInfo>
read_derivation_info(NixContext &ctx, const nix::StorePath &drv_path) {
  try {
    auto drv = ctx.store()->readDerivation(drv_path);

    DerivationInfo info{drv_path};
    info.name = drv.name;
    info.system = drv.platform;
    // info.attrs = drv.structuredAttrs->structuredAttrs;
    // info.env = drv.env;

    for (const auto &[input_drv, _] : drv.inputDrvs.map)
      info.input_drvs.push_back(input_drv);

    for (const auto &[output_name, output] : drv.outputs)
      if (auto path = output.path(*ctx.store_ptr(), drv.name, output_name))
        info.output_paths.push_back(*path);

    return info;
  } catch (...) {
    return std::nullopt;
  }
}

/// Get input derivations for a derivation path.
/// Returns empty vector on error.
inline std::vector<nix::StorePath>
get_input_drvs(NixContext &ctx, const nix::StorePath &drv_path) {
  try {
    auto drv = ctx.store()->readDerivation(drv_path);
    std::vector<nix::StorePath> inputs;
    for (const auto &[input_drv, _] : drv.inputDrvs.map)
      inputs.push_back(input_drv);
    return inputs;
  } catch (...) {
    return {};
  }
}

/// Build a dependency map for a set of derivations.
/// Returns map from drv path string to list of input drv path strings.
inline std::map<std::string, std::vector<std::string>>
build_dependency_graph(NixContext &ctx,
                       const std::vector<nix::StorePath> &drv_paths) {
  std::map<std::string, std::vector<std::string>> deps;

  for (const auto &path : drv_paths) {
    std::string path_str{path.to_string()};
    deps[path_str] = {};

    for (const auto &input : get_input_drvs(ctx, path))
      deps[path_str].push_back(std::string{input.to_string()});
  }

  return deps;
}

/// Try to get the build derivation path for a store path.
/// For output paths, this returns the .drv that produced them.
inline std::optional<nix::StorePath> get_build_drv(NixContext &ctx,
                                                   const nix::StorePath &path) {
  try {
    return ctx.store()->getBuildDerivationPath(path);
  } catch (...) {
    return std::nullopt;
  }
}

// ============================================================================
// Flake resolution
// ============================================================================

/// Resolve a flake installable (e.g. ".#default") to derivation paths.
inline std::vector<nix::StorePath>
resolve_installable(NixContext &ctx, const std::string &installable) {
  auto store_ref = ctx.store();
  auto eval_state = ctx.eval_state();

  auto fetch_settings = nix::fetchers::Settings{};
  fetch_settings.allowDirty = true;
  fetch_settings.warnDirty = false;

  auto [flake_ref, fragment] = nix::parseFlakeRefWithFragment(
      fetch_settings, installable, std::filesystem::current_path().string());

  nix::flake::LockFlags lock_flags;
  lock_flags.failOnUnlocked = false;
  lock_flags.allowUnlocked = true;
  lock_flags.requireLockable = false;
  lock_flags.writeLockFile = false;

  nix::ExtendedOutputsSpec outputs_spec = nix::ExtendedOutputsSpec::Default();

  nix::Strings defaults = {"packages." + nix::settings.thisSystem.get() +
                               ".default",
                           "defaultPackage." + nix::settings.thisSystem.get()};
  nix::Strings prefixes = {"packages." + nix::settings.thisSystem.get() + ".",
                           "legacyPackages." + nix::settings.thisSystem.get() +
                               "."};

  auto inst = nix::make_ref<nix::InstallableFlake>(
      nullptr, eval_state, std::move(flake_ref), fragment, outputs_spec,
      defaults, prefixes, lock_flags);

  nix::Installables installables = {inst};
  auto drv_set = nix::Installable::toDerivations(store_ref, installables, true);

  return std::vector<nix::StorePath>(drv_set.begin(), drv_set.end());
}

/// Resolve a flake installable to derivation JSON strings.
inline std::vector<std::string>
show_derivation(NixContext &ctx, const std::string &installable) {
  std::vector<std::string> result;
  for (const auto &drv_path : resolve_installable(ctx, installable)) {
    auto drv = ctx.store()->readDerivation(drv_path);
    result.push_back(drv.toJSON().dump(2));
  }
  return result;
}

} // namespace nxb
