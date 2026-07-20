{
  description = "CrossPoint Reader development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-compat.url = "github:NixOS/flake-compat";
  };

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          # Detect the project root from wherever the user entered the shell,
          # so commands work from the repository root or any subdirectory.
          # Using `git rev-parse` to do so (assuming git is installed
          # system-wide); user can overwrite this by setting PROJECT_ROOT env.
          setEnvs = ''
            PROJECT_ROOT="''${PROJECT_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
            export PROJECT_ROOT
            export PLATFORMIO_CORE_DIR="$PROJECT_ROOT/.cache/platformio"
          '';
          fhsEnv = pkgs.buildFHSEnv {
            name = "crosspoint-reader-shell";

            targetPkgs =
              pkgs: with pkgs; [
                python3
                uv

                # Runtime libraries used by PlatformIO's downloaded ESP32 toolchain binaries.
                stdenv.cc.cc.lib
                zlib
                ncurses
              ];

            profile = ''
              ${setEnvs}
              export PATH="$PROJECT_ROOT/.venv/bin:$PATH"

              if [ ! -x "$PROJECT_ROOT/.venv/bin/pio" ]; then
                echo "Creating .venv and installing pioarduino PlatformIO Core..."
                # Forcing python3 from fhsEnv, otherwise we get the following
                # exception while running `pip check`
                # ModuleNotFoundError: No module named 'littlefs'
                uv venv --python /usr/bin/python3 "$PROJECT_ROOT/.venv" &&
                uv pip install --python "$PROJECT_ROOT/.venv/bin/python" \
                  -U https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip \
                  -r "$PROJECT_ROOT/requirements.txt" ||
                echo "Failed to install pioarduino PlatformIO Core" >&2
              fi
            '';
          };
          pio = pkgs.writeShellScriptBin "pio" ''
            exec ${fhsEnv}/bin/crosspoint-reader-shell -c 'exec pio "$@"' pio "$@"
          '';
        in
        {
          default = pkgs.mkShell {
            packages = [
              pio
              fhsEnv
            ];

            shellHook = setEnvs;
          };
        }
      );
    };
}
