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
            ];
            shellHook = ''
              unset MACOSX_DEPLOYMENT_TARGET

              store_include=$(pkg-config --variable=includedir nix-store 2>/dev/null || true)
              util_include=$(pkg-config --variable=includedir nix-util 2>/dev/null || true)

              if [ -n "$store_include" ] && [ -n "$util_include" ]; then
                export NIX_API_INPUTS="''${store_include}/nix/store ''${util_include}/nix/util"
              fi

              if [ -n "$store_include" ] && [ -n "$util_include" ]; then
                export NIX_API_INCLUDE_PATH="''${store_include} ''${util_include}"
              fi
            '';
          };
        });
    };
}
