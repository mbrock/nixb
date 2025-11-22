{
  description = "Dev shell for nixb (meson + clang toolchain)";

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
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          emacsWithPkgs = (pkgs.emacsPackagesFor pkgs.emacs).emacsWithPackages (
            epkgs: with epkgs; [
              eat
              yaml
            ]
          );
        in
        rec {
          inherit (pkgs) hello;
          emacs = emacsWithPkgs;
          nxb-ttytest = pkgs.writeShellApplication {
            name = "nxb-ttytest";
            runtimeInputs = [
              pkgs.nix.dev
              pkgs.git
              pkgs.meson
              pkgs.ninja
              pkgs.pkg-config
              pkgs.cmake
              pkgs.clang
              pkgs.python3
              emacsWithPkgs
            ];
            text = ''
              set -euo pipefail

              usage() {
                cat <<'EOF' >&2
              Usage: nxb-ttytest [YAML_FILE...]

              Build and run terminal tests described in YAML using Emacs/EAT.
              If no files are provided, defaults to src/new/test/tests.yaml.
              EOF
                exit 1
              }

              if [ "$#" -eq 0 ]; then
                set -- src/new/test/tests.yaml
              fi

              files=("$@")
              list=""
              for f in "''${files[@]}"; do
                list+="$f"$'\n'
              done

              export NXB_TTYTEST_FILES="''${list}"

              root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
              cd "$root"

              : "''${EMACS:=emacs}"
              "$EMACS" -Q -batch -l ert \
                -l "$root/src/new/test/nxb-term-tests.el" \
                --eval "(nxb-run-yaml-tests nil)"
            '';
          };
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShellNoCC {
            buildInputs = with pkgs; [
              nix.dev
              pkg-config
              boost
            ];
            packages = with pkgs; [
              meson
              ninja
              cmake
              python3
              git
              nixd
              treefmt
            ];
            shellHook = ''
              unset MACOSX_DEPLOYMENT_TARGET

              store_include=$(pkg-config --variable=includedir nix-store 2>/dev/null)
              util_include=$(pkg-config --variable=includedir nix-util 2>/dev/null)
              flake_include=$(pkg-config --variable=includedir nix-flake 2>/dev/null)
              expr_include=$(pkg-config --variable=includedir nix-expr 2>/dev/null)
              cmd_include=$(pkg-config --variable=includedir nix-cmd 2>/dev/null)

              export NIX_API_INPUTS="''${store_include}/nix/store ''${util_include}/nix/util ''${flake_include}/nix/flake ''${expr_include}/nix/expr ''${cmd_include}/nix/cmd"
              export NIX_API_INCLUDE_PATH="''${store_include} ''${util_include} ''${flake_include} ''${expr_include} ''${cmd_include}"
            '';
          };
        }
      );
    };
}
