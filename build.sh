#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

CONFIG=ReleaseFast
TARGET=x86_64-linux-gnu.2.17  # glibc 2.17 baseline for Linux releases

set_config() {
    case "${1:-release}" in
        debug)   CONFIG=Debug ;;
        release) CONFIG=ReleaseFast ;;
        *)       echo "Unknown config: $1"; exit 1 ;;
    esac
}

project_name() {
    sed -n 's/^const name = "\([A-Za-z0-9_-]*\)".*/\1/p' build.zig
}

run() {
    set_config "${1:-debug}"
    unset WATCH  # ignore an inherited WATCH value
    zig build run -Doptimize=$CONFIG -Dtarget=$TARGET
}

dev() {
    export WATCH=1  # enable file watching
    zig build run -Doptimize=Debug -Dtarget=$TARGET
}

linux() {
    set_config "${1:-}"
    zig build -Doptimize=$CONFIG -Dtarget=$TARGET
}

windows() {
    set_config "${1:-}"
    zig build -Doptimize=$CONFIG -Dtarget=x86_64-windows-gnu
}

web() {
    zig build web -Dweb
    echo "test locally: emrun zig-out/web/index.html"
}

dist() {
    rm -rf dist

    linux
    windows
    web

    local name="$(project_name)"
    mkdir -p dist
    zip -j "dist/$name-linux.zip" "zig-out/bin/$name" zig-out/bin/*.pak
    zip -j "dist/$name-windows.zip" "zig-out/bin/$name.exe" zig-out/bin/*.pak
    zip -j "dist/$name-web.zip" zig-out/web/index.html \
        zig-out/web/index.js zig-out/web/index.wasm zig-out/web/index.data
    echo "dist/ ready: $name-linux.zip  $name-windows.zip  $name-web.zip"
}

bindgen() {
    zig build bindgen
}

clean() {
    rm -rf zig-out .zig-cache dist
}

show_help() {
    echo "Usage: $0 <command> [options]"
    echo "Commands:"
    echo "  run       Build and run the game [debug|release]"
    echo "  dev       Build and run the game with hot reload"
    echo "  linux     Build the linux target [debug|release]"
    echo "  windows   Build the windows target [debug|release]"
    echo "  web       Build the web target"
    echo "  dist      Package release zips for all platforms into dist/"
    echo "  bindgen   Regenerate the raylib bindings and Luau type definitions"
    echo "  clean     Clean build environment"
    echo "  help      Show this help message"
    echo ""
    echo "Build targets default to release, run defaults to debug."
}

case "${1:-help}" in
    run|linux|windows)
        [ "$#" -le 2 ] || { echo "$1 takes at most one option"; exit 1; }
        "$1" "${2:-}" ;;
    dev|web|dist|bindgen|clean)
        [ "$#" -le 1 ] || { echo "$1 takes no options"; exit 1; }
        "$1" ;;
    help|--help|-h) show_help ;;
    *) echo "Unknown command: $1"; show_help; exit 1 ;;
esac
