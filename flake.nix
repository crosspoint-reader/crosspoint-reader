{
  description = "CrossPoint Reader - Firmware for Xteink X4 e-paper display";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      perSystem = { config, self', pkgs, lib, system, ... }:
        let
          # Version from platformio.ini
          version = "0.12.0";

          # Create a platformio wrapper with all required dependencies
          platformio = pkgs.platformio-core.overrideAttrs (old: {
            propagatedBuildInputs = (old.propagatedBuildInputs or [ ]) ++ [
              pkgs.python3Packages.packaging
            ];
          });

          # Python with required packages for build scripts
          pythonEnv = pkgs.python3.withPackages (ps: [ ps.pip ]);

          # Python environment for fontconvert script
          fontconvertPython = pkgs.python3.withPackages (ps: [ ps.freetype-py ]);

          # Wrapper script for fontconvert.py
          fontconvertScript = pkgs.writeShellScriptBin "fontconvert" ''
            exec ${fontconvertPython}/bin/python3 ${./lib/EpdFont/scripts/fontconvert.py} "$@"
          '';

          # Script to convert all builtin fonts
          convertBuiltinFontsScript = pkgs.writeShellScriptBin "convert-builtin-fonts" ''
            set -e
            if [ ! -d "lib/EpdFont/scripts" ]; then
              echo "Error: must be run from the project root (where platformio.ini is)"
              exit 1
            fi
            export PATH="${fontconvertPython}/bin:$PATH"
            cd lib/EpdFont/scripts
            exec ${pkgs.bash}/bin/bash ./convert-builtin-fonts.sh
          '';

          # Script to build font IDs (requires Ruby for SHA256 hashing)
          buildFontIdsScript = pkgs.writeShellScriptBin "build-font-ids" ''
            set -e
            if [ ! -d "lib/EpdFont/scripts" ]; then
              echo "Error: must be run from the project root (where platformio.ini is)"
              exit 1
            fi
            export PATH="${pkgs.ruby}/bin:$PATH"
            cd lib/EpdFont/scripts
            exec ${pkgs.bash}/bin/bash ./build-font-ids.sh "$@"
          '';

          # Combined script to regenerate all fonts (run after editing fontconvert.py)
          regenerateFontsScript = pkgs.writeShellScriptBin "regenerate-fonts" ''
            set -e
            if [ ! -d "lib/EpdFont/scripts" ]; then
              echo "Error: must be run from the project root (where platformio.ini is)"
              exit 1
            fi
            export PATH="${fontconvertPython}/bin:${pkgs.ruby}/bin:$PATH"
            echo "Regenerating all builtin fonts..."
            cd lib/EpdFont/scripts
            ${pkgs.bash}/bin/bash ./convert-builtin-fonts.sh
            echo ""
            echo "Building font IDs..."
            ${pkgs.bash}/bin/bash ./build-font-ids.sh > ../builtinFonts/font_ids.h
            echo "Generated lib/EpdFont/builtinFonts/font_ids.h"
            echo ""
            echo "Done! All fonts regenerated."
          '';

          # FHS environment to run PlatformIO with dynamically linked toolchains
          fhsEnv = pkgs.buildFHSEnv {
            name = "pio-env";
            targetPkgs = pkgs: [
              platformio
              pythonEnv
              pkgs.git
              pkgs.zlib
              pkgs.stdenv.cc.cc.lib
            ];
            runScript = "bash";
          };

          # Build script that runs inside FHS environment
          buildScript = pkgs.writeShellScriptBin "build-firmware" ''
            set -e
            echo "Building CrossPoint Reader firmware..."
            echo ""

            # Ensure we're in the project directory
            if [ ! -f platformio.ini ]; then
              echo "Error: platformio.ini not found. Run this from the project root."
              exit 1
            fi

            # Run the build inside FHS environment
            ${fhsEnv}/bin/pio-env -c "pio run -e gh_release"

            echo ""
            echo "Build complete! Firmware at: .pio/build/gh_release/firmware.bin"
          '';

          # Wrapper for pio that runs inside FHS environment
          pioWrapper = pkgs.writeShellScriptBin "pio" ''
            exec ${fhsEnv}/bin/pio-env -c "pio $*"
          '';
        in
        {
          packages = {
            default = buildScript;
            build-script = buildScript;
          };

          devShells.default = pkgs.mkShell {
            packages = [
              # Build tools (wrapped for FHS compatibility)
              fhsEnv
              pioWrapper
              buildScript
              pythonEnv

              pkgs.clang-tools
              pkgs.python3Packages.python-lsp-server
              pkgs.nixd
              pkgs.esptool
            ];

            shellHook = ''
              echo "CrossPoint Reader development environment"
              echo ""
              echo "Build commands:"
              echo "  pio run               - Build firmware (default env, via FHS wrapper)"
              echo "  pio run -e gh_release - Build release firmware"
              echo "  pio run --target upload - Flash to device"
              echo "  build-firmware        - Build release firmware directly"
              echo ""
              echo "Enter FHS shell for direct PlatformIO access:"
              echo "  pio-env               - Enter FHS shell with PlatformIO"
              echo ""
              echo "Language servers available:"
              echo "  clangd   - C/C++ LSP"
              echo "  pylsp    - Python LSP"
              echo "  nixd     - Nix LSP"
            '';
          };
          apps = {
            build = {
              type = "app";
              program = "${buildScript}/bin/build-firmware";
            };

            fontconvert = {
              type = "app";
              program = "${fontconvertScript}/bin/fontconvert";
            };

            convert-builtin-fonts = {
              type = "app";
              program = "${convertBuiltinFontsScript}/bin/convert-builtin-fonts";
            };

            build-font-ids = {
              type = "app";
              program = "${buildFontIdsScript}/bin/build-font-ids";
            };

            regenerate-fonts = {
              type = "app";
              program = "${regenerateFontsScript}/bin/regenerate-fonts";
            };
          };
        };
    };
}
