{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    platformio
    python3
    git
  ];

  shellHook = ''
    echo "PlatformIO development environment loaded"
    echo "Run 'pio run' to build the firmware"
    echo "Run 'pio run -t upload' to build and flash"
  '';
}
