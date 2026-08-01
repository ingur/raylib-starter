#pragma once

#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace vfs {

// installs raylib's file callbacks and mounts ASSETS_PAK, loose files win.
// reads see the pak and loose files, writes are restricted to save/
bool Mount();
void Unmount();

// true when every segment is non-empty and not "." or "..", so joining the
// path to a directory cannot climb back out of it
bool PlainPath(std::string_view path);

// nullopt when the file does not exist
std::optional<std::string> Read(std::string_view path);

// resolves a script module under game/
std::optional<std::string> ReadScript(std::string_view path);

// adds the host file and music loaders to the raylib table, after OpenRaylib
void OpenLoaders(lua_State *L);

}  // namespace vfs
