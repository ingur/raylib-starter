#!/usr/bin/env bash
# Build helper, a thin wrapper over the CMake presets.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# --- Utility Functions ---

CONFIG=Release

set_config() {
    case "${1:-release}" in
        debug)   CONFIG=Debug ;;
        release) CONFIG=Release ;;
        *)       echo "Unknown config: $1"; exit 1 ;;
    esac
}

# configure once per target, after that ninja reconfigures itself when needed
target() {
    local preset=$1 config=$2
    [ -f "build/$preset/build.ninja" ] || cmake --preset "$preset"
    cmake --build "build/$preset" --config "$config" ${3:+--target "$3"}
}

project_name() {
    sed -n 's/^project(\([A-Za-z0-9_-]*\).*/\1/p' CMakeLists.txt
}

# --- Commands ---

run() {
    set_config "${1:-debug}"
    unset WATCH  # run never watches, dev does
    target linux "$CONFIG" run
}

dev() {
    export WATCH=1  # hot reload, see WatchFiles in src/main.c
    target linux Debug run
}

linux() {
    set_config "${1:-}"
    target linux "$CONFIG"
}

windows() {
    set_config "${1:-}"
    target windows "$CONFIG"
}

web() {
    # emsdk installs set EMSDK, the web preset expects EMSCRIPTEN
    if [ -z "${EMSCRIPTEN:-}" ] && [ -n "${EMSDK:-}" ]; then
        export EMSCRIPTEN="$EMSDK/upstream/emscripten"
    fi
    set_config "${1:-}"
    target web "$CONFIG"
    echo "test locally: emrun build/web/$CONFIG/$(project_name).html"
}

dist() {
    linux
    windows
    web

    local name="$(project_name)"
    rm -rf dist && mkdir -p dist
    zip -j "dist/$name-linux.zip" "build/linux/Release/$name" build/linux/Release/*.pak
    zip -j "dist/$name-windows.zip" "build/windows/Release/$name.exe" build/windows/Release/*.pak
    cp "build/web/Release/$name.html" build/web/Release/index.html
    zip -j "dist/$name-web.zip" build/web/Release/index.html \
        "build/web/Release/$name.js" "build/web/Release/$name.wasm" "build/web/Release/$name.data"
    echo "dist/ ready: $name-linux.zip  $name-windows.zip  $name-web.zip"
}

bindgen() {
    [ -f build/linux/build.ninja ] || cmake --preset linux
    python3 tools/bindgen.py build/linux/_deps/raylib-src build/linux/_deps/pocketpy-src
}

clean() {
    rm -rf build dist
}

# --- Command Handling ---

show_help() {
    echo "Usage: $0 <command> [options]"
    echo "Commands:"
    echo "  run       Build and run the game [debug|release]"
    echo "  dev       Build and run the game with hot reload"
    echo "  linux     Build the linux target [debug|release]"
    echo "  windows   Build the windows target [debug|release]"
    echo "  web       Build the web target [debug|release]"
    echo "  dist      Package release zips for all platforms into dist/"
    echo "  bindgen   Regenerate the python bindings and type stubs"
    echo "  clean     Clean build environment"
    echo "  help      Show this help message"
    echo ""
    echo "Build targets default to release, run defaults to debug."
}

case "${1:-help}" in
    run|linux|windows|web)
        [ "$#" -le 2 ] || { echo "$1 takes at most one option"; exit 1; }
        "$1" "${2:-}" ;;
    dev|dist|bindgen|clean)
        [ "$#" -le 1 ] || { echo "$1 takes no options"; exit 1; }
        "$1" ;;
    help|--help|-h) show_help ;;
    *) echo "Unknown command: $1"; show_help; exit 1 ;;
esac
