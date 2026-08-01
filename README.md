# raylib-starter

A minimal [raylib](https://github.com/raysan5/raylib) 6.0 game template using [Luau](https://github.com/luau-lang/luau) 0.732 with a C++17 host.

- Linux x64, Windows x64, and web targets
- Native code generation on desktop
- Hot reload
- Generated raylib bindings and Luau definitions
- Zip asset packing with loose-file overrides
- Persistent `save/` storage, backed by IndexedDB on web

## Requirements

- Linux or WSL2
- `git`, `zip`, and Zig 0.16
- `pkg-config`, OpenGL, X11, and Wayland development libraries
- Emscripten for web builds
- `curl` and Python 3 for `./build.sh bindgen`
- Optional: [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) for editor support

See the [system dependencies](https://github.com/ingur/raylib-starter/wiki/System-dependencies) page for installation commands.

## Start

```bash
git clone --depth 1 https://github.com/ingur/raylib-starter.git
cd raylib-starter
./build.sh run
```

The first build downloads and compiles raylib and Luau. Later builds are incremental. Build output goes to `zig-out/`.

Useful commands:

```bash
./build.sh dev
./build.sh run release
./build.sh help
```

`run` defaults to a debug build. Other build commands default to release.

## Game scripts

- `game/main.luau` is the entry point
- Define `update()` to run code every frame
- Return `true` from `update()` to quit
- `game/window.luau` contains startup settings
- `require("name")` loads and caches `game/name.luau`
- Debug builds set the `DEV` global
- `before_reload()` may return a state string
- `after_reload(state)` receives that string after a successful reload
- Hot reload does not release GPU or audio resources. Unload them in `before_reload()`

## raylib bindings

Use the global table through a local name:

```luau
local rl = raylib
```

- `Vector2` and `Vector3` use Luau's native `vector` type
- `Color` is a packed `0xRRGGBBAA` number
- Other supported raylib structs use userdata with field access
- Resource handles have no script constructor
- Enums and colors are flat values such as `rl.KEY_SPACE` and `rl.RAYWHITE`
- Functions that cannot be exposed safely are absent
- `./build.sh bindgen` prints the unsupported list

The template does not bind every raylib function.

## Assets and saves

- `assets/` and `game/` are packed into `assets.pak`, which is a zip file
- Loose files override files in the pak
- `rl.LoadFileText` returns a string
- `rl.LoadFileData` returns a buffer
- Both loaders raise an error when the file is missing
- Write persistent files under `save/`
- Files outside `save/` are temporary on web

## Editor setup

- VS Code uses the committed `.vscode/settings.json`
- Neovim uses the committed `.nvim.lua` after `vim.o.exrc = true`
- Other editors can pass `--definitions:@raylib=<repo>/types/raylib.d.luau` to luau-lsp
- Set luau-lsp `platform.type` to `standard`

## Project settings

- Change the binary name in `build.zig`
- Change dependency versions in `build.zig.zon`
- Run `./build.sh bindgen` after changing the raylib version

Previous implementations remain on the [`luajit`](https://github.com/ingur/raylib-starter/tree/luajit) and [`pocketpy`](https://github.com/ingur/raylib-starter/tree/pocketpy) branches.
