<p align="center">
  <img src="https://github.com/ingur/raylib-starter/assets/45173070/ae6b5749-c53d-470d-8dc2-36460d37ac5a"/>
</p>

<h1 align="center">raylib-starter</h1>

<p align="center">
  My personal, minimal game template for creating cross-platform
  <a href="https://github.com/raysan5/raylib">raylib</a> 
  games using LuaJIT (and C/C++).
</p>

## Features
* Includes pre-built LuaJIT and raylib ([v5.5](https://github.com/raysan5/raylib/releases/tag/5.5))
* C/C++ and LuaJIT interoperability using `ffi`
* Cross-platform builds (Linux-x64 & Windows-x64)
* Includes build helper script for easy development
* Configurable automatic asset packing/loading using custom zip-files

> NOTE: the old C/C++ only version with basic web target support (no LuaJIT) can be found [here](https://github.com/ingur/raylib-starter/tree/websupport)

## Requirements
* `git`, `cmake`, `g++`, `zip`
* `mingw-w64` for the windows target

## Getting Started

Clone the repository:
```bash
git clone --depth 1 https://github.com/ingur/raylib-starter.git
```

Use the helper script to initialize the build files and run the game:
```bash
./build.sh run
```

You can use the following commands:
```bash
# usage: ./build.sh <command> [options]
./build.sh init             # re-initalizes all build files using CMake

./build.sh linux_x86_64     # builds linux target [debug|release] [zip]
./build.sh windows_x86_64   # builds windows target [debug|release] [zip]
./build.sh all              # build for all platforms and types [zip]

./build.sh run              # builds and runs default target [debug|release] [zip]

./build.sh clean            # cleans build environment for all targets
./build.sh help             # shows the help message
```
> [!NOTE]
> When adding/removing C++ source files, the build files should be re-initialized using `./build.sh init`

## Example Code
```lua
rl.SetConfigFlags(rl.FLAG_VSYNC_HINT)

rl.InitWindow(800, 450, "basic window")

while not rl.WindowShouldClose() do
	rl.BeginDrawing()

	rl.ClearBackground(rl.RAYWHITE)
	rl.DrawText("Congrats! You created your first window!", 190, 200, 20, rl.LIGHTGRAY)

	rl.EndDrawing()
end

rl.CloseWindow()
```

## Configuration/Tips

* Default lua entrypoint is set to `lua/main.lua`
* Basic autocompletion support is available through the `lua/defs.lua` file ([source](https://github.com/TSnake41/raylib-lua/blob/master/tools/autocomplete/plugin.lua))
* Project name / source files can be configured in `CMakeLists.txt`
* Asset packing format/structure can be configured in `build.sh`
* Assets/files can also be loaded through the virtual filesystem, e.g:
  ```lua
  local texture = rl.LoadTexture("assets/texture.png")
  local library = require("library") -- looks in lua/ archive by default
  ```

## Credits

* [raylib](https://github.com/raysan5/raylib) for the amazing library
* [raylib-lua](https://github.com/TSnake41/raylib-lua) for LuaJIT-based raylib bindings
