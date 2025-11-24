{
  description = "Dev shell for nixb (cmake + clang toolchain)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
            packages = with pkgs; [
              nix.dev
              libblake3.dev
              libsodium.dev
              brotli.dev
              libcpuid
              curl.dev
              libseccomp.dev
              sqlite.dev
              libgit2.dev
              pcre2.dev
              lowdown.dev
              editline.dev
              c-ares.dev
              nix.dev
              boost
              pkg-config
              cmake
              python3
              git
              nixd
              treefmt
              cmake-language-server
            ];
            shellHook = ''
              unset MACOSX_DEPLOYMENT_TARGET

              store_include=$(pkg-config --variable=includedir nix-store 2>/dev/null)
              util_include=$(pkg-config --variable=includedir nix-util 2>/dev/null)
              flake_include=$(pkg-config --variable=includedir nix-flake 2>/dev/null)
              expr_include=$(pkg-config --variable=includedir nix-expr 2>/dev/null)
              cmd_include=$(pkg-config --variable=includedir nix-cmd 2>/dev/null)
              main_include=$(pkg-config --variable=includedir nix-main 2>/dev/null)
              cares_include=$(pkg-config --variable=includedir c-ares 2>/dev/null)

              export NIX_API_INPUTS="''${store_include}/nix/store ''${util_include}/nix/util ''${flake_include}/nix/flake ''${expr_include}/nix/expr ''${cmd_include}/nix/cmd ''${main_include}/nix/main ''${cares_include}"
              export NIX_API_INCLUDE_PATH="''${store_include} ''${util_include} ''${flake_include} ''${expr_include} ''${cmd_include} ''${main_include} ''${cares_include}"
            '';
          };
        }
      );
    };
}
