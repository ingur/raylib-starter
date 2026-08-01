{ pkgs, lib, ... }:

{
  languages.zig.enable = true;
  languages.zig.version = "0.16.0";

  languages.cplusplus.enable = true;
  languages.cplusplus.lsp.package = pkgs.clang-tools;

  packages = [
    pkgs.git
    pkgs.curl
    pkgs.zip
    pkgs.pkg-config
    pkgs.emscripten

    # luau-lsp uses the generated definitions:
    # luau-lsp analyze --definitions=types/raylib.d.luau game/*.luau
    # the game embeds the Luau version pinned in build.zig.zon
    pkgs.luau
    pkgs.luau-lsp

    pkgs.python3

    pkgs.libGL
    pkgs.wayland
    pkgs.wayland-protocols
    pkgs.wayland-scanner
    pkgs.libxkbcommon
    pkgs.libdecor
    pkgs.xorg.libX11
    pkgs.xorg.libXrandr
    pkgs.xorg.libXinerama
    pkgs.xorg.libXcursor
    pkgs.xorg.libXi
    pkgs.xorg.libXrender
    pkgs.xorg.libXfixes
    pkgs.xorg.libXext
  ];

  # GLFW dlopens these at runtime
  env.LD_LIBRARY_PATH = lib.makeLibraryPath [
    pkgs.libGL
    pkgs.wayland
    pkgs.libxkbcommon
    pkgs.libdecor
    pkgs.xorg.libX11
    pkgs.xorg.libXrandr
    pkgs.xorg.libXinerama
    pkgs.xorg.libXcursor
    pkgs.xorg.libXi
  ] + ":/run/opengl-driver/lib";

  enterShell = ''
    echo "commands: ./build.sh help"
  '';
}
