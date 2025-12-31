{
  description = "CrossPoint Reader - ESP32 E-Paper Firmware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            platformio
            python3
            git
          ];

          shellHook = ''
            echo "CrossPoint Reader development environment"
            echo "Commands:"
            echo "  pio run           - Build firmware"
            echo "  pio run -t upload - Build and flash to device"
            echo "  pio run -t clean  - Clean build artifacts"
          '';
        };
      }
    );
}
