#pragma once

#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace vfs {

// Installs raylib's LoadFileData/LoadFileText callbacks and mounts ASSETS_PAK
// (beside the executable natively, "/" ASSETS_PAK on web). Loose files always win.
bool Mount();
void Unmount();

// Reads a file through the vfs (loose file first, then the pak).
// Returns nullopt when the file does not exist. Contents are NOT NUL-padded for you;
// std::string already is.
std::optional<std::string> Read(std::string_view path);

// Resolves a script module: tries "game/<path>" first, then "<path>".
std::optional<std::string> ReadScript(std::string_view path);

// Registers LoadFileText / LoadFileData onto the existing global `raylib` table.
// Must be called after OpenRaylib.
void OpenLoaders(lua_State *L);

}  // namespace vfs
