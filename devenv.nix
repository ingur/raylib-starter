{ pkgs, lib, ... }:

{
  # zig builds every native target, zls comes along for build.zig
  languages.zig.enable = true;
  languages.zig.version = "0.16.0";

  # clangd for the C++ host, it reads the generated compile_flags.txt
  languages.cplusplus.enable = true;
  languages.cplusplus.lsp.package = pkgs.clang-tools;

  packages = [
    pkgs.git
    pkgs.curl
    pkgs.zip
    pkgs.pkg-config
    pkgs.emscripten

    # luau-lsp drives editor completion from types/raylib.d.lua and doubles as a
    # checker: luau-lsp analyze --definitions=types/raylib.d.lua game/*.lua.
    # Note these track nixpkgs (luau 0.726) while the game embeds the version
    # pinned in build.zig.zon (0.732), built from source. The pin is what runs.
    pkgs.luau
    pkgs.luau-lsp

    # bindgen is a plain python script, it needs no third party packages
    pkgs.python3

    # linux window and GL libs for raylib's GLFW (X11 and Wayland)
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

  # GLFW dlopens these at runtime.
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
