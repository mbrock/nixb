{
  description = "Dev shell for nixb (cmake + clang toolchain)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.detnix.url = "github:DeterminateSystems/nix-src";

  outputs =
    { nixpkgs, detnix, ... }:
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
            hardeningDisable = [ "all" ];
            packages = with pkgs; [
              detnix.packages.${system}.nix.dev

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
              liburing.dev

              c-ares.dev
              boost
              pkg-config
              cmake
              python3
              git
              nixd
              treefmt
              cmake-language-server
              ccache
              mold
            ];
          };
        }
      );
    };
}
