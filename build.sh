#!/bin/bash

function build_init() {
  # create build directories if they do not exist
  mkdir -p build/web
  mkdir -p build/linux
  mkdir -p build/windows

  # configure web target
  cd build/web
  emcmake cmake ../..
  cd ../..

  # configure linux target
  cd build/linux
  cmake -DPLATFORM=linux ../..
  cd ../..

  # configure windows target
  cd build/windows
  cmake -DCMAKE_TOOLCHAIN_FILE=../../mingw-toolchain.cmake -DPLATFORM=windows ../..
  cd ../..
}

function build {
  cd build/$1
  make
  cd ../..
}

function build_all {
  build linux
  build windows
  build web
}

function clean {
    for dir in linux windows web; do
        if [ -d build/$dir ]; then
            echo "Cleaning $dir build..."
            cd build/$dir
            make clean
            cd ../..
        fi
    done
}

function watch {
    # start Browser-Sync in the background and watch files in the build/web directory
    npx --yes browser-sync start --server 'build/web' --files 'build/web' --open "local" --no-notify --port 8000 &
    SERVER_PID=$!

    # use nodemon to watch the src directory and rebuild on change
    npx --yes nodemon --watch src -e cpp,hpp --exec "bash build.sh web"

    # stop Browser-Sync when nodemon exits
    kill $SERVER_PID
}

if [ $# -eq 0 ]; then
    echo "No arguments provided. Use 'init', 'clean', 'watch', 'all', 'linux', 'windows', or 'web'."
    exit 1
fi

case "$1" in
    init) build_init;;
    all) build_all;;
    web) build web;;
    linux) build linux;;
    windows) build windows;;
    clean) clean;;
    watch) watch;;
    *) echo "Invalid argument: $1. Use 'init', 'clean', 'watch', 'all', 'linux', 'windows', or 'web'.";;
esac

echo "Done."
