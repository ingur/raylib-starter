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
- `require("name")` loads and caches `game/name.luau`, nested paths work as `require("module/path")`
- Debug builds set the `DEV` global
- `before_reload()` may return a table of plain data
- `after_reload(state)` receives that table after a successful reload
- Reload state holds booleans, numbers, strings, vectors, buffers and nested tables. Anything else is reported and the state is dropped
- Hot reload does not release GPU or audio resources. Unload them in `before_reload()`

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
- `rl.FileExists`, `rl.DirectoryExists`, `rl.GetFileLength`, `rl.GetFileModTime` and `rl.LoadDirectoryFiles` answer for loose files and the pak
- Everything else raylib offers for files is bound as raylib defines it
- raylib 6.0's own `FileMove` copies but never removes the source, use `rl.FileCopy` then `rl.FileRemove`

## Models

- `rl.LoadModel` and `rl.LoadModelAnimations` read through the virtual filesystem
- `rl.LoadModelAnimations` returns a table of handles, pass it back to `rl.UnloadModelAnimations`
- glTF, GLB and IQM load fully from the pak, including their animations
- An obj that declares materials must stay a loose file, raylib opens the mtl and its textures itself
- Debug builds abort on m3d, raylib's bundled parser reads unaligned and Zig traps that

## Limitations

The binding answers for what it introduces, the Luau side of a value, the life
of a handle, an open scope, and anything the virtual filesystem moves. raylib
keeps its own parameter contracts, so a call that is wrong in C is wrong here
for the same reason and in the same way.

What C raylib does that a script here cannot. Every case is listed with its
reason in the report at the end of `src/bind/raylib_bind.cpp`.

- The audio stream callback and the mixed processors. They run on raylib's audio thread and one Luau VM is not thread safe
- `SetTraceLogCallback`, a variadic C callback, and the four file callback setters, which are how the virtual filesystem is installed
- Calls taking an array of values, the splines and the triangle strips. Loop the bound single element calls, `DrawSplineSegment*`, `DrawLineV`, `DrawTriangle`
- `DrawMeshInstanced`, which has no single element equivalent
- Writing into a raylib buffer, `UpdateAudioStream`, `UpdateSound`, `UpdateMeshBuffer`, `UpdateTextureRec`, `SetShaderValueV`. Scripts cannot synthesise audio or stream vertices
- Calls returning an owning pointer, `LoadImageColors` and `LoadCodepoints` among them. Luau's `buffer`, `string` and `utf8` cover the usual reasons to want them
- The `Text*` helpers that build a new string, `TextFormat`, `TextSplit`, `TextToUpper` and the rest. Luau's `string` and `utf8` do this natively, the ones that only read, such as `TextLength` and `TextSubtext`, are bound
- The compression, encoding and hashing helpers, which Luau has no equivalent for

These are what the generic binding cannot express. A project can add a purpose
built adapter beside the generated code where that is feasible, which it is not
for the callbacks, since the audio thread cannot enter the one Luau VM.

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
