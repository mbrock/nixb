#pragma once

#include <fmt/color.h>
#include <fmt/core.h>

#include <functional>
#include <map>
#include <mutex>
#include <nix/store/realisation.hh>
#include <nix/store/store-api.hh>
#include <nix/util/archive.hh>
#include <nix/util/callback.hh>
#include <nix/util/hash.hh>
#include <nix/util/memory-source-accessor.hh>
#include <nix/util/ref.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>
#include <ranges>
#include <vector>

#include "nxt/app.hpp"

namespace nxb {

struct TrivialStoreConfig
    : public std::enable_shared_from_this<TrivialStoreConfig>,
      virtual nix::StoreConfig
{
    TrivialStoreConfig(const Params & params)
        : StoreConfig(params)
    {
    }

    static const std::string name()
    {
        return "Trivial Store";
    }

    static nix::StringSet uriSchemes()
    {
        return {"trivial"};
    }

    nix::ref<nix::Store> openStore() const override;

    nix::StoreReference getReference() const override
    {
        return {
            .variant = nix::StoreReference::Specified{.scheme = "trivial"},
            .params = getQueryParams(),
        };
    }
};

struct TrivialStore : virtual nix::Store
{
    using Config = TrivialStoreConfig;
    nix::ref<const Config> config;

    ui::UIRuntime & runtime;

    struct StoredPath
    {
        nix::UnkeyedValidPathInfo info;
        std::shared_ptr<nix::MemorySourceAccessor> contents;

        StoredPath(
            nix::UnkeyedValidPathInfo i,
            std::shared_ptr<nix::MemorySourceAccessor> c)
            : info(std::move(i))
            , contents(std::move(c))
        {
        }
    };

    std::map<nix::StorePath, StoredPath> storage;

    mutable std::mutex storage_mutex;

    TrivialStore(nix::ref<const Config> config, ui::UIRuntime & runtime)
        : nix::Store{*config}
        , config(config)
        , runtime(runtime)
    {
    }

    void print(std::string_view msg)
    {
        runtime.println(std::string(msg));
    }

    std::string getUri()
    {
        return "trivial://";
    }

    // === Query operations ===

    void queryPathInfoUncached(
        const nix::StorePath & path,
        nix::Callback<std::shared_ptr<const nix::ValidPathInfo>>
            callback) noexcept override
    {
        std::lock_guard lock(storage_mutex);
        auto it = storage.find(path);
        if (it != storage.end()) {
            callback(
                std::make_shared<nix::ValidPathInfo>(
                    nix::StorePath{path}, it->second.info));
        } else {
            callback(nullptr);
        }
    }

    void queryRealisationUncached(
        const nix::DrvOutput &,
        nix::Callback<std::shared_ptr<const nix::UnkeyedRealisation>>
            callback) noexcept override
    {
        callback(nullptr);
    }

    std::optional<nix::StorePath>
    queryPathFromHashPart(const std::string & hashPart) override
    {
        std::lock_guard lock(storage_mutex);
        for (const auto & [path, _] : storage) {
            if (path.hashPart() == hashPart)
                return path;
        }
        return std::nullopt;
    }

    // === Storage operations ===

    void addToStore(
        const nix::ValidPathInfo & info,
        nix::Source & source,
        nix::RepairFlag,
        nix::CheckSigsFlag) override
    {
        print(fmt::format("addToStore: {}", printStorePath(info.path)));

        // Parse NAR into memory
        auto contents = nix::make_ref<nix::MemorySourceAccessor>();
        nix::MemorySink sink{*contents};
        nix::parseDump(sink, source);

        std::lock_guard lock(storage_mutex);
        storage.emplace(info.path, StoredPath{info, contents});

        print(
            fmt::format(
                "  stored {} ({} refs)",
                info.path.name(),
                info.references.size()));
    }

    nix::StorePath addToStoreFromDump(
        nix::Source & source,
        std::string_view name,
        nix::FileSerialisationMethod dumpMethod,
        nix::ContentAddressMethod hashMethod,
        nix::HashAlgorithm hashAlgo,
        const nix::StorePathSet & refs,
        nix::RepairFlag,
        std::shared_ptr<const nix::Provenance>) override
    {
        print(
            fmt::format(
                "addToStoreFromDump: {} ({} refs)", name, refs.size()));

        // Read dump into memory accessor
        auto contents = nix::make_ref<nix::MemorySourceAccessor>();
        {
            nix::MemorySink sink{*contents};
            switch (dumpMethod) {
            case nix::FileSerialisationMethod::NixArchive:
                nix::parseDump(sink, source);
                break;
            case nix::FileSerialisationMethod::Flat:
                contents->root = nix::MemorySourceAccessor::File::Regular{};
                sink.createRegularFile(nix::CanonPath::root, [&](auto & s) {
                    source.drainInto(s);
                });
                break;
            }
        }

        // Compute hash
        auto hash = nix::hashPath(
                        {contents, nix::CanonPath::root},
                        hashMethod.getFileIngestionMethod(),
                        hashAlgo)
                        .first;
        auto narHash = nix::hashPath(
            {contents, nix::CanonPath::root},
            nix::FileIngestionMethod::NixArchive,
            nix::HashAlgorithm::SHA256);

        // Create path info
        auto info = nix::ValidPathInfo::makeFromCA(
            *this,
            name,
            nix::ContentAddressWithReferences::fromParts(
                hashMethod,
                std::move(hash),
                {.others = refs, .self = false}),
            std::move(narHash.first));
        info.narSize = narHash.second.value();

        auto path = info.path;

        // Store it
        {
            std::lock_guard lock(storage_mutex);
            storage.emplace(path, StoredPath{std::move(info), contents});
        }

        print(fmt::format("  stored: {}", printStorePath(path)));
        return path;
    }

    void registerDrvOutput(const nix::Realisation & output) override
    {
        print(fmt::format("registerDrvOutput: {}", output.id.to_string()));
        // TODO: store realisations too if needed
    }

    // === Read operations ===

    void narFromPath(const nix::StorePath & path, nix::Sink & sink) override
    {
        std::lock_guard lock(storage_mutex);
        auto it = storage.find(path);
        if (it == storage.end())
            throw nix::Error(
                "path '%s' is not valid", printStorePath(path));

        nix::SourcePath sourcePath(
            nix::ref<nix::SourceAccessor>(it->second.contents));
        nix::dumpPath(
            sourcePath, sink, nix::FileSerialisationMethod::NixArchive);
    }

    nix::ref<nix::SourceAccessor> getFSAccessor(bool) override
    {
        std::vector<nix::ref<nix::SourceAccessor>> layers;
        for (auto & [_, p] : storage)
            layers.push_back(nix::ref(p.contents));
        return nix::ref(nix::makeUnionSourceAccessor(std::move(layers)));
    }

    std::shared_ptr<nix::SourceAccessor>
    getFSAccessor(const nix::StorePath & path, bool) override
    {
        std::lock_guard lock(storage_mutex);
        auto it = storage.find(path);
        if (it != storage.end())
            return it->second.contents;
        return nullptr;
    }

    std::optional<nix::TrustedFlag> isTrustedClient() override
    {
        return nix::Trusted;
    }

    // === Stats ===
    size_t storedPathCount() const
    {
        std::lock_guard lock(storage_mutex);
        return storage.size();
    }
};

// Note: openStore() requires runtime, so we implement it via
// makeTrivialStore
inline nix::ref<nix::Store> TrivialStoreConfig::openStore() const
{
    throw nix::Error(
        "TrivialStore requires UIRuntime - use makeTrivialStore()");
}

// Helper to create a TrivialStore with UIRuntime reference
inline std::shared_ptr<TrivialStore>
makeTrivialStore(ui::UIRuntime & runtime)
{
    auto config =
        std::make_shared<TrivialStoreConfig>(nix::StoreConfig::Params{});
    return std::make_shared<TrivialStore>(
        nix::ref<const TrivialStoreConfig>(config), runtime);
}

} // namespace nxb
