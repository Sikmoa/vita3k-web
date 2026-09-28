{
  description = "Vita3K Web development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
      };
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          # Build tools
          git
          cmake
          ninja
          pkg-config
          python3

          # Native compiler/toolchain
          clang
          lld

          # Native Vita3K dependencies
          openssl
          boost
          sdl3

          # Qt 6.11.x
          qt6.qtbase
          qt6.qtmultimedia
          qt6.qtsvg
          qt6.qttools

          # Web / Wasm
          emscripten
          nodejs_24

          # Wasm inspection/debugging
          binaryen
          wabt
        ];

        shellHook = ''
          export CC=clang
          export CXX=clang++

          # Keep Emscripten's cache local to this project/user rather
          # than relying on a read-only Nix store cache.
          export EM_CACHE="$PWD/.cache/emscripten"
          export EM_FROZEN_CACHE=0
          mkdir -p "$EM_CACHE"

          echo
          echo "Vita3K Web development shell"
          echo "--------------------------------"
          echo "Qt:         $(qmake6 -query QT_VERSION 2>/dev/null || true)"
          echo "Node:       $(node --version)"
          echo "Emscripten: $(emcc --version | head -n1)"
          echo "Clang:      $(clang --version | head -n1)"
          echo "CMake:      $(cmake --version | head -n1)"
          echo "Ninja:      $(ninja --version)"
          echo
        '';
      };
    };
}
