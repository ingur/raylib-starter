{ pkgs, lib, ... }:

{
  # clang and clangd for the C host.
  languages.cplusplus.enable = true;
  languages.cplusplus.lsp.package = pkgs.clang-tools;

  packages = [
    pkgs.git
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.zip
    pkgs.emscripten                    # web
    pkgs.pkgsCross.mingwW64.stdenv.cc  # windows cross compiler

    # python tooling: LSP for game scripts, deps for bindgen
    pkgs.basedpyright
    (pkgs.python3.withPackages (ps: [ ps.pycparser ps.pcpp ]))

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
  ];

  env.EMSCRIPTEN = "${pkgs.emscripten}/share/emscripten";

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
