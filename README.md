# Raylib game template
My personal, minimal game template for creating a modern Raylib game using C++ on linux using CMake.

## Features
* Linux builds
* Windows builds using mingw
* Web builds using emscripten
* Hot-reloading web builds using node.js
* Build helper script for easy development

## Requirements
* `git`, `cmake`, `g++`
* [emscripten](https://emscripten.org/docs/getting_started/downloads.html) (`emcmake`) for the web target
* `mingw-w64` for the windows target
* [npm](https://nodejs.org/) for hot-reloading 

## Getting Started
* Clone the repository:
```bash
git clone --depth 1 https://github.com/ingur/raylib-starter.git
```
* Use the helper script to initialize the build files:
```bash
./build.sh init
```
* You can now use the following commands:
```bash
./build.sh all # builds for linux, windows and web
./build.sh init # re-initalizes all build files using CMake

./build.sh web # builds web target
./build.sh linux # builds linux target
./build.sh windows # builds windows target

./build.sh clean # runs `make clean` for all targets

./build.sh watch # starts hot-reloading for the web target
```
> NOTE: when adding/removing source files, the build files need to be re-initialized using `./build.sh init`!

