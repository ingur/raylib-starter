#include "script.hpp"

#include "raylib_bind.hpp"
#include "vfs.hpp"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#if defined(GAME_CODEGEN)
#include "luacodegen.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

namespace {

// registry slots, one set per VM
const char *const kModuleCache = "game.modules";
const char *const kTraceback = "game.traceback";

// unique address parked in the cache while a module is running, so a module
// that requires itself fails loudly instead of recursing forever
char kLoading;

// Compiles source and leaves the chunk on the stack. On failure the error
// string is left on the stack instead, matching luau_load's own contract.
bool LoadChunk(lua_State *L, const char *chunkname, const std::string &source) {
    lua_CompileOptions opts = {};
    opts.optimizationLevel = 2;
    opts.debugLevel = 1;
    opts.typeInfoLevel = 1;
    opts.userdataTypes = kRaylibUserdataTypes;
    opts.vectorLib = "vector";
    opts.vectorCtor = "create";
    opts.vectorType = "vector";

    size_t bcsize = 0;
    std::unique_ptr<char, void (*)(void *)> bc(
        luau_compile(source.data(), source.size(), &opts, &bcsize), std::free);
    if (!bc) {
        lua_pushstring(L, "out of memory while compiling");
        return false;
    }
    if (luau_load(L, chunkname, bc.get(), bcsize, 0) != 0) return false;
#if defined(GAME_CODEGEN)
    luau_codegen_compile(L, -1);
#endif
    return true;
}

// pcall error handler: the traceback is only meaningful before the stack
// unwinds, so it has to be grabbed here rather than after lua_pcall returns
int Traceback(lua_State *L) {
    const char *message = lua_tostring(L, 1);
    std::string out = message != nullptr ? message : "unknown error";
    if (const char *trace = lua_debugtrace(L)) {
        out += '\n';
        out += trace;
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// host replacement for Luau's require: resolves game/<name>.lua through the
// vfs, runs it once and caches whatever it returned
int HostRequire(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_settop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, kModuleCache);  // 2: cache
    lua_rawgetfield(L, 2, name);                       // 3: cached value
    if (lua_islightuserdata(L, 3) && lua_tolightuserdata(L, 3) == &kLoading)
        luaL_error(L, "module '%s' requires itself", name);
    if (!lua_isnil(L, 3)) return 1;
    lua_pop(L, 1);

    std::string path(name);
    path += ".lua";
    std::optional<std::string> source = vfs::ReadScript(path);
    if (!source) luaL_error(L, "module '%s' not found", name);

    std::string chunkname = "=game/";
    chunkname += path;
    if (!LoadChunk(L, chunkname.c_str(), *source)) lua_error(L);  // 3: chunk, or the error

    lua_pushlightuserdata(L, &kLoading);
    lua_rawsetfield(L, 2, name);

    lua_call(L, 0, 1);  // errors propagate to the enclosing pcall with the traceback intact

    if (lua_isnil(L, -1)) {
        lua_pushnil(L);
        lua_rawsetfield(L, 2, name);  // drop the sentinel so a fixed module can load later
        luaL_error(L, "module '%s' returned no value", name);
    }
    lua_pushvalue(L, -1);
    lua_rawsetfield(L, 2, name);
    return 1;
}

}  // namespace

Script::~Script() { Close(); }

void Script::Close() {
    if (L != nullptr) {
        lua_close(L);
        L = nullptr;
    }
    entry.clear();
    ResetResult();
}

bool Script::Reset(bool devMode) {
    Close();
    lastError[0] = '\0';

    L = luaL_newstate();
    if (L == nullptr) {
        std::snprintf(lastError, sizeof(lastError), "failed to create the lua state");
        std::fprintf(stderr, "%s\n", lastError);
        return false;
    }
#if defined(GAME_CODEGEN)
    if (luau_codegen_supported()) luau_codegen_create(L);
#endif
    luaL_openlibs(L);

    lua_pushboolean(L, devMode);
    lua_setglobal(L, "DEV");

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kModuleCache);

    lua_pushcfunction(L, Traceback, "traceback");
    lua_setfield(L, LUA_REGISTRYINDEX, kTraceback);

    lua_pushcfunction(L, HostRequire, "require");
    lua_setglobal(L, "require");

    OpenRaylib(L);
    vfs::OpenLoaders(L);
    return true;
}

void Script::ResetResult() {
    lastString.clear();
    lastIsString = false;
    lastNil = true;
    lastTruthy = false;
}

void Script::CaptureResult() {
    lastNil = lua_isnil(L, -1);
    lastTruthy = lua_toboolean(L, -1) != 0;
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t len = 0;
        const char *text = lua_tolstring(L, -1, &len);
        lastString.assign(text, len);
        lastIsString = true;
    }
    lua_pop(L, 1);
}

void Script::CaptureError() {
    const char *message = lua_tostring(L, -1);
    std::snprintf(lastError, sizeof(lastError), "%s", message != nullptr ? message : "unknown error");
    lua_pop(L, 1);
    std::fprintf(stderr, "%s\n", lastError);
}

bool Script::Pcall(int nargs, int nresults) {
    int base = lua_gettop(L) - nargs;  // the function being called
    lua_getfield(L, LUA_REGISTRYINDEX, kTraceback);
    lua_insert(L, base);
    int status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (status != LUA_OK) {
        CaptureError();
        return false;
    }
    return true;
}

// Reset() only fails when the allocator does, but the boot path carries on so
// the error screen can say so; every call has to survive a dead vm.
bool Script::NoState() {
    if (L != nullptr) return false;
    std::snprintf(lastError, sizeof(lastError), "the lua state is not running");
    return true;
}

bool Script::RunEntry(const char *path, bool *missing) {
    if (missing != nullptr) *missing = false;
    ResetResult();
    if (NoState()) return false;

    std::optional<std::string> source = vfs::ReadScript(path);
    if (!source) {
        if (missing != nullptr) *missing = true;
        std::snprintf(lastError, sizeof(lastError), "game/%s not found", path);
        return false;
    }

    entry = "game/";
    entry += path;
    std::string chunkname = "=";
    chunkname += entry;
    if (!LoadChunk(L, chunkname.c_str(), *source)) {
        CaptureError();
        return false;
    }
    return Pcall(0, 0);
}

bool Script::CallGlobal(const char *name, bool *missing) {
    if (missing != nullptr) *missing = false;
    ResetResult();
    if (NoState()) return false;

    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (missing != nullptr) *missing = true;
        if (entry.empty())
            std::snprintf(lastError, sizeof(lastError), "%s() is not defined", name);
        else
            std::snprintf(lastError, sizeof(lastError), "%s() is not defined in %s", name, entry.c_str());
        return false;
    }
    if (!Pcall(0, 1)) return false;
    CaptureResult();
    return true;
}

bool Script::CallGlobalStr(const char *name, const char *arg, bool *missing) {
    if (missing != nullptr) *missing = false;
    ResetResult();
    if (NoState()) return false;

    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (missing != nullptr) *missing = true;
        std::snprintf(lastError, sizeof(lastError), "%s() is not defined", name);
        return false;
    }
    lua_pushstring(L, arg);
    if (!Pcall(1, 1)) return false;
    CaptureResult();
    return true;
}

bool Script::Require(const char *name, bool *missing) {
    if (missing != nullptr) *missing = false;
    ResetResult();
    if (NoState()) return false;

    // probe first: a module that is simply absent is not an error for callers
    // like the window config, and require() itself can only raise
    std::string path(name);
    path += ".lua";
    if (!vfs::ReadScript(path)) {
        if (missing != nullptr) *missing = true;
        std::snprintf(lastError, sizeof(lastError), "game/%s not found", path.c_str());
        return false;
    }

    lua_getglobal(L, "require");
    lua_pushstring(L, name);
    return Pcall(1, 1);
}
