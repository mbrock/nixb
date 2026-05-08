{
  description = "nixb - Nix build UI";

  inputs = {
    nix-src.url = "path:./nix-src";
    nixpkgs.follows = "nix-src/nixpkgs";
  };

  outputs =
    { self, nix-src, nixpkgs, ... }:
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
          inherit (pkgs.stdenv) mkDerivation;

            ccacheConfig = ''
              export CCACHE_DIR=/nix/var/cache/ccache
              export CCACHE_UMASK=007
              export CCACHE_COMPRESS=1
            '';

          # Custom dependencies not in nixpkgs
          libcoro = mkDerivation {
            pname = "libcoro";
            version = "0.16.0";
            src = ./vendor/libcoro;
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [ pkgs.openssl ];
            cmakeFlags = [
              "-DLIBCORO_BUILD_TESTS=OFF"
              "-DLIBCORO_BUILD_EXAMPLES=OFF"
              "-DLIBCORO_EXTERNAL_DEPENDENCIES=OFF"
              "-DLIBCORO_FEATURE_NETWORKING=ON"
              "-DLIBCORO_FEATURE_TLS=ON"
            ];
          };

          mp-units = mkDerivation {
            pname = "mp-units";
            version = "2.4.0";
            src = pkgs.fetchFromGitHub {
              owner = "mpusz";
              repo = "mp-units";
              rev = "v2.4.0";
              hash = "sha256-BlemzDArgAvCA4o+G2YPG0D+ISMWJsys57MMfugBURo=";
            };
            sourceRoot = "source/src";
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [ fmt
                            pkgs.gmp pkgs.gsl-lite ];
            cmakeFlags = [
              "-DCMAKE_CXX_STANDARD=20"
              "-DMP_UNITS_API_STD_FORMAT=OFF"
              "-DMP_UNITS_API_CONTRACTS=NONE"
              "-DMP_UNITS_BUILD_INSTALL=OFF"
            ];
          };

          fmtVersion = { version, hash } : with pkgs;
              stdenv.mkDerivation {
      pname = "fmt";
      inherit version;

      outputs = [
        "out"
        "dev"
      ];

      src = fetchFromGitHub {
        owner = "fmtlib";
        repo = "fmt";
        rev = version;
        inherit hash;
      };

      nativeBuildInputs = [ cmake ];

      cmakeFlags = [ (lib.cmakeBool "BUILD_SHARED_LIBS" true) ];

      doCheck = true;

      passthru.tests = {
        inherit
          mpd
          openimageio
          fcitx5
          spdlog
          ;
      };

      meta = with lib; {
        description = "Small, safe and fast formatting library";
        longDescription = ''
          fmt (formerly cppformat) is an open-source formatting library. It can be
          used as a fast and safe alternative to printf and IOStreams.
        '';
        homepage = "https://fmt.dev/";
        changelog = "https://github.com/fmtlib/fmt/blob/${version}/ChangeLog.rst";
        downloadPage = "https://github.com/fmtlib/fmt/";
        maintainers = [ ];
        license = licenses.mit;
        platforms = platforms.all;
      };
    };

  fmt_12 = fmtVersion {
    version = "12.0.0";
    hash = "sha256-AZDmIeU1HbadC+K0TIAGogvVnxt0oE9U6ocpawIgl6g=";
  };

  fmt = fmt_12;


          # Base on nix-src's dev shell, add our deps
          nixDevShell = nix-src.devShells.${system}.native;
        in
        {
#          shellHook = ''
#            ${ccacheConfig}
#            export CC=clang
#            export CXX=clang++
#          '';
          default = pkgs.mkShell.override { stdenv = nixDevShell.stdenv; } {
            inputsFrom = [ nixDevShell ];
            hardeningDisable = ["all"];
            packages = [
              pkgs.lld

              libcoro
              pkgs.c-ares
              pkgs.openssl
              mp-units
              fmt
              pkgs.cli11
              pkgs.duckdb

              pkgs.nixd
#              pkgs.llvmPackages_20.clang
              pkgs.nixfmt-rfc-style
#              pkgs.llvmPackages_20.clang-tools
              pkgs.treefmt
            ];
          };
        }
      );
    };
}
