{
  description = "Dev shell for nixb (meson + clang toolchain)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              meson
              ninja
              cmake
              pkg-config
              clang
              clang-tools
              python3
              git
            ];

            shellHook = ''
              export CC=clang
              export CXX=clang++
            '';
          };
        });
    };
}
