---
trigger: always_on
---

If you need to do the meson setup step (`make setup`) due to changing the meson.build, you'll need to do it with `nix develop` commands to get all the build inputs for the Nix C++ libraries. Normal builds can be done with just `make` which calls meson compile with `build` as the build directory.