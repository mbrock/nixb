## Nix C++ Store API Synopsis

### Initialization

```cpp
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/store/derivations.hh>

nix::initLibStore();
auto store = nix::openStore();
```

### Store Path Parsing & Manipulation

```cpp
// Parse path from string
StorePath path = store->parseStorePath("/nix/store/abc123...-hello-2.10");

// Safe parsing (returns std::optional)
auto maybePath = store->maybeParseStorePath("/nix/store/...");

// Check if string is a store path
bool valid = store->isStorePath("/nix/store/...");

// Convert back to string
std::string fullPath = store->printStorePath(path);

// Extract components from StorePath
std::string_view name = path.name();          // "hello-2.10"
std::string_view hash = path.hashPart();      // first 32 chars
std::string_view basename = path.to_string(); // "abc123...-hello-2.10"
bool isDrv = path.isDerivation();             // ends with .drv?

// Split path with subpaths
auto [storePath, subPath] = store->toStorePath("/nix/store/abc-foo/bin/bar");
// storePath = "/nix/store/abc-foo", subPath = "/bin/bar"
```

### Querying Path Information

```cpp
// Check if path is valid in store
bool valid = store->isValidPath(path);

// Get full metadata about a path
ref<const ValidPathInfo> info = store->queryPathInfo(path);
// info->deriver       - optional<StorePath> to .drv that built this
// info->references    - StorePathSet of dependencies
// info->narSize       - uint64_t size in bytes
// info->registrationTime - time_t when added to store

// Get paths that reference this path
StorePathSet referrers;
store->queryReferrers(path, referrers);

// Get closure (all transitive dependencies)
StorePathSet closure;
store->computeFSClosure(path, closure);
// Optional flags: flipDirection, includeOutputs, includeDerivers
```

### Working with Derivations

```cpp
// Read a .drv file
Derivation drv = store->readDerivation(drvPath);

// drv.outputs          - map<string, DerivationOutput>
//                        keys: "out", "dev", "bin", etc.
// drv.inputDrvs        - map<StorePath, StringSet>
//                        input derivations and which outputs needed
// drv.inputSrcs        - StorePathSet of source files
// drv.platform         - "x86_64-linux", etc.
// drv.builder          - path to builder executable
// drv.args             - vector<string> of arguments
// drv.env              - map<string, string> of environment vars

// Query derivation outputs
OutputPathMap outputs = store->queryDerivationOutputMap(drvPath);
// map<string, StorePath> - "out" -> /nix/store/...

// Check if path is a derivation
if (path.isDerivation()) {
    auto drv = store->readDerivation(path);
}
```

### Key Types

```cpp
// StorePath - opaque handle to a store path (just the basename)
//   - constructor: StorePath(std::string_view baseName)
//   - .name() -> name part
//   - .hashPart() -> hash part
//   - .to_string() -> full basename
//   - .isDerivation() -> bool

// StorePathSet - std::set<StorePath>
// StorePaths - std::vector<StorePath>

// ValidPathInfo - metadata about a path
//   - .deriver, .references, .narSize, .registrationTime

// Derivation - parsed .drv file
//   - .outputs, .inputDrvs, .inputSrcs, .builder, .args, .env
```

### Utilities

```cpp
// Print set of paths (human-readable with quotes/commas)
std::string pretty = store->showPaths(pathSet);

// Follow symlinks to store
StorePath resolved = store->followLinksToStorePath("/some/symlink");

// Check if path is in store
bool inStore = store->isInStore("/nix/store/abc-foo/bin");

// Topological sort (dependencies before dependents)
StorePaths sorted = store->topoSortPaths(pathSet);
```

### Common Pattern for Build Monitor

```cpp
// From JSON log, get a store path string
std::string pathStr = "/nix/store/xyz123-hello-2.10";

// Parse it
auto path = store->parseStorePath(pathStr);

// Get name for display
fmt::print("Building: {}\n", path.name());

// If it's a derivation, get details
if (path.isDerivation()) {
    auto drv = store->readDerivation(path);
    fmt::print("Builder: {}\n", drv.builder);
    fmt::print("Platform: {}\n", drv.platform);

    // Get dependencies
    for (auto& [inputDrv, outputs] : drv.inputDrvs) {
        fmt::print("  depends on: {}\n", store->printStorePath(inputDrv));
    }
}
```

That's the essential API surface you'll need!
