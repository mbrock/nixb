{
  description = "Dev shell for nixb (meson + clang toolchain)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          emacsWithPkgs = (pkgs.emacsPackagesFor pkgs.emacs).emacsWithPackages (epkgs:
            with epkgs; [
              vterm
              magit
              consult
              vertico
              orderless
              marginalia
              embark
              embark-consult
              corfu
              cape
              eglot
              which-key
              doom-themes
            ]);
        in
        {
          inherit (pkgs) hello;
          emacs = emacsWithPkgs;
        });
        
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShellNoCC {
            buildInputs = with pkgs; [
              nix.dev pkg-config boost
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
        });
    };
}
