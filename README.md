<p align="center">
  <img src="https://github.com/ingur/raylib-starter/assets/45173070/ae6b5749-c53d-470d-8dc2-36460d37ac5a"/>
</p>

<h1 align="center">raylib-starter</h1>

<p align="center">
  My personal, minimal game template for creating cross-platform
  <a href="https://github.com/raysan5/raylib">raylib</a>
  games in <a href="https://github.com/luau-lang/luau">Luau</a>, with a C++17 host.
</p>

## Features

- raylib ([6.0](https://github.com/raysan5/raylib/releases/tag/6.0)) and Luau ([0.732](https://github.com/luau-lang/luau/releases/tag/0.732)), built from source and version pinned
- Linux x64, Windows x64, and web targets
- Native code generation on desktop
- Hot reload
- Generated raylib bindings and Luau definitions
- Zip asset packing with loose-file overrides
- Persistent `save/` storage, backed by IndexedDB on web

> [!NOTE]
> The previous LuaJIT (and C/C++) version lives on the [`luajit`](https://github.com/ingur/raylib-starter/tree/luajit) branch, and the previous pocketpy version on the [`pocketpy`](https://github.com/ingur/raylib-starter/tree/pocketpy) branch.

## Requirements

- Linux or WSL2
- `git`, `zip`, and Zig 0.16
- `pkg-config`, OpenGL, X11, and Wayland development libraries
- Emscripten for web builds
- `curl` and Python 3 for `./build.sh bindgen`
- Optional: [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) for editor support

> [!TIP]
> See the [system dependencies](https://github.com/ingur/raylib-starter/wiki/System-dependencies) page for installation commands.

## Start

```bash
git clone --depth 1 https://github.com/ingur/raylib-starter.git
cd raylib-starter
./build.sh run
```

> [!NOTE]
> The first build downloads and compiles raylib and Luau. Later builds are incremental. Build output goes to `zig-out/`.

Useful commands:

```bash
./build.sh dev
./build.sh run release
./build.sh help
```

`run` defaults to a debug build. Other build commands default to release.

## Example

```luau
local rl = raylib

-- top level runs once at boot, the window is already open.
-- define update(), it runs every frame (return true to quit)
function update()
    rl.BeginDrawing()

    rl.ClearBackground(rl.RAYWHITE)
    rl.DrawText("Congrats! You created your first window!", 190, 200, 20, rl.LIGHTGRAY)

    rl.EndDrawing()
end
```

## Game scripts

- `game/main.luau` is the entry point
- Define `update()` to run code every frame
- Return `true` from `update()` to quit
- `game/window.luau` contains startup settings
- `require("name")` loads and caches `game/name.luau`, nested paths work as `require("module/path")`
- Debug builds set the `DEV` global
- `before_reload()` may return a table of plain data
- `after_reload(state)` receives that table after a successful reload
- Reload state holds booleans, numbers, strings, vectors, buffers and nested tables. Anything else is reported and the state is dropped
- Hot reload does not release GPU or audio resources. Unload them in `before_reload()`
- Reloads run `main.luau` again, so guard one-time setup like `rl.InitAudioDevice()` with `rl.IsAudioDeviceReady()`
- `rl.SetShapesTexture` survives reloads, reset it with `rl.SetShapesTexture(tex, rl.Rectangle())` before unloading the texture

## raylib bindings

Use the global table through a local name:

```luau
local rl = raylib
```

- `Vector2` and `Vector3` use Luau's native `vector` type
- `Color` is a packed `0xRRGGBBAA` number
- raymath is bound, so `rl.MatrixTranslate` and `rl.QuaternionSlerp` handle 3D transforms
- Use the `vector` library and operators for vector maths, they allocate nothing
- Other supported raylib structs use userdata with field access
- Resource handles have no script constructor
- Enums and colors are flat values such as `rl.KEY_SPACE` and `rl.RAYWHITE`
- `rl.LoadShader` takes no nil, pass `""` to keep raylib's default shader for that slot
- Functions that cannot be exposed safely are absent
- `./build.sh bindgen` regenerates the bindings and the unsupported report

The template does not bind every raylib function.

## Assets and saves

- `assets/` and `game/` are packed into `assets.pak`, which is a zip file
- Reads see loose files first, then the pak, so a loose file overrides a packed one
- Writes go wherever you point them, raylib's own rules apply
- `save/` is the one directory the web build keeps, so put saves there to stay portable
- `rl.LoadFileText` returns a string, `rl.LoadFileData` returns a buffer
- Both loaders raise an error when the file is missing
- `rl.SaveFileText` and `rl.SaveFileData` create parent directories on the way
- Exports and `rl.TakeScreenshot` follow the same rules, except `.bmp` and `.qoi` images, which write straight to disk and never persist on web
- `rl.FileExists`, `rl.DirectoryExists`, `rl.GetFileLength`, `rl.GetFileModTime` and `rl.LoadDirectoryFiles` answer for loose files and the pak
- Everything else raylib offers for files is bound as raylib defines it
- `rl.ChangeDirectory` moves loose reads, writes and the dev watcher with it, capture `rl.GetWorkingDirectory()` and restore it
- raylib 6.0's `FileMove` deletes the source when the copy fails, use `rl.FileCopy` then `rl.FileRemove`

## Models

- `rl.LoadModel` and `rl.LoadModelAnimations` read through the virtual filesystem
- `rl.LoadModelAnimations` returns a table of handles, pass it back to `rl.UnloadModelAnimations`
- glTF, GLB and IQM load fully from the pak, including their animations
- An obj that declares materials must stay a loose file, raylib opens the mtl and its textures itself
- `rl.SetMaterialTexture` hands the texture to the material, `rl.UnloadMaterial` frees it, do not also `rl.UnloadTexture` it
- Textures a model file loads stay allocated until exit, scripts cannot reach them to unload
- Debug builds abort on m3d, raylib's bundled parser reads unaligned and Zig traps that

## Limitations

Every case is listed with its reason in the report at the end of `src/bind/raylib_bind.cpp`.

- The audio stream callback and the mixed processors. They run on raylib's audio thread and one Luau VM is not thread safe
- `SetTraceLogCallback`, a variadic C callback, and the four file callback setters, which are how the virtual filesystem is installed
- The window lifecycle and the automation events. The host owns the window, and recording needs a list pointer raylib keeps past the call
- Calls taking an array of values, the splines and the triangle strips. Loop the bound single element calls, `DrawSplineSegment*`, `DrawLineV`, `DrawTriangle`
- `DrawMeshInstanced`, which has no single element equivalent
- Writing into a raylib buffer, `UpdateAudioStream`, `UpdateSound`, `UpdateMeshBuffer`, `UpdateTextureRec`, `SetShaderValueV`. Scripts cannot synthesise audio or stream vertices
- Calls returning an owning pointer, `LoadImageColors` and `LoadCodepoints` among them. Luau's `buffer`, `string` and `utf8` cover the usual reasons to want them
- The `Text*` helpers that build a new string, `TextFormat`, `TextSplit`, `TextToUpper` and the rest. Luau's `string` and `utf8` do this natively, the ones that only read, such as `TextLength` and `TextSubtext`, are bound
- The compression, encoding and hashing helpers, which Luau has no equivalent for

## Editor setup

- VS Code uses the committed `.vscode/settings.json`
- Neovim uses the committed `.nvim.lua` after `vim.o.exrc = true`
- Other editors can pass `--definitions:@raylib=<repo>/types/raylib.d.luau` to luau-lsp
- Set luau-lsp `platform.type` to `standard`
- Check scripts without an editor with `luau-lsp analyze --definitions=types/raylib.d.luau game/*.luau`

## Project settings

- Change the binary name in `build.zig`
- Change dependency versions in `build.zig.zon`
- Run `./build.sh bindgen` after changing the raylib version

## Credits

- [raylib](https://github.com/raysan5/raylib) for the amazing library
- [Luau](https://github.com/luau-lang/luau) for the fast, embeddable scripting language
- [miniz](https://github.com/richgel999/miniz) for zip-based asset packing
