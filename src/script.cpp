#include "script.hpp"

#include "bind/raylib_bind.hpp"
#include "bind/rl_adapters.hpp"
#include "vfs.hpp"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#if defined(GAME_CODEGEN)
#include "luacodegen.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace {

// registry slots, one set per VM
const char *const kModuleCache = "game.modules";
const char *const kTraceback = "game.traceback";

// unique address parked in the cache while a module is running, so a module
// that requires itself fails loudly instead of recursing forever
char kLoading;

// a reload carries plain data only. a resource handle would outlive the unload
// adapter that owns it, and a function cannot be rebuilt
constexpr int kMaxStateDepth = 64;

struct Snapshotter {
    std::string error;
    std::vector<const void *> seen;  // every table, so sharing and cycles both fail

    bool Take(lua_State *L, int idx, ScriptState &out, std::string &path, int depth);

private:
    bool Reject(const std::string &path, const char *what) {
        error = path + " is " + what;
        return false;
    }
};

bool Snapshotter::Take(lua_State *L, int idx, ScriptState &out, std::string &path, int depth) {
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            out.kind = ScriptState::Kind::Bool;
            out.boolean = lua_toboolean(L, idx) != 0;
            return true;
        case LUA_TNUMBER:
            out.kind = ScriptState::Kind::Number;
            out.number = lua_tonumber(L, idx);
            return true;
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char *text = lua_tolstring(L, idx, &len);
            out.kind = ScriptState::Kind::String;
            out.bytes.assign(text, len);
            return true;
        }
        case LUA_TVECTOR: {
            const float *v = lua_tovector(L, idx);
            out.kind = ScriptState::Kind::Vector;
            out.vec[0] = v[0];
            out.vec[1] = v[1];
            out.vec[2] = v[2];
            return true;
        }
        case LUA_TBUFFER: {
            std::size_t len = 0;
            const char *data = static_cast<const char *>(lua_tobuffer(L, idx, &len));
            out.kind = ScriptState::Kind::Buffer;
            out.bytes.assign(data, len);
            return true;
        }
        case LUA_TTABLE:
            break;
        default:
            return Reject(path, (std::string("a ") + luaL_typename(L, idx)).c_str());
    }

    if (depth >= kMaxStateDepth) return Reject(path, "nested too deeply");
    if (lua_getmetatable(L, idx)) {
        lua_pop(L, 1);
        return Reject(path, "a table with a metatable");
    }

    const void *identity = lua_topointer(L, idx);
    for (const void *other : seen)
        if (other == identity) return Reject(path, "a table that appears twice in the state");
    seen.push_back(identity);

    out.kind = ScriptState::Kind::Table;
    const int table = lua_absindex(L, idx);
    for (int iter = lua_rawiter(L, table, 0); iter >= 0; iter = lua_rawiter(L, table, iter)) {
        const int keyType = lua_type(L, -2);
        if (keyType != LUA_TSTRING && keyType != LUA_TNUMBER) {
            const std::string what = std::string("keyed by a ") + luaL_typename(L, -2);
            lua_pop(L, 2);
            return Reject(path, what.c_str());
        }

        // lua_tostring would convert a numeric key in place and derail the iterator
        std::string child = path;
        if (keyType == LUA_TSTRING) {
            child += '.';
            child += lua_tostring(L, -2);
        } else {
            char index[32];
            std::snprintf(index, sizeof(index), "[%.14g]", lua_tonumber(L, -2));
            child += index;
        }

        out.pairs.emplace_back();
        auto &entry = out.pairs.back();
        const bool ok = Take(L, -2, entry.first, child, depth + 1) &&
                        Take(L, -1, entry.second, child, depth + 1);
        if (!ok) {
            lua_pop(L, 2);
            return false;
        }
        lua_pop(L, 2);
    }
    return true;
}

void PushState(lua_State *L, const ScriptState &state) {
    switch (state.kind) {
        case ScriptState::Kind::Bool:
            lua_pushboolean(L, state.boolean);
            return;
        case ScriptState::Kind::Number:
            lua_pushnumber(L, state.number);
            return;
        case ScriptState::Kind::String:
            lua_pushlstring(L, state.bytes.data(), state.bytes.size());
            return;
        case ScriptState::Kind::Vector:
            lua_pushvector(L, state.vec[0], state.vec[1], state.vec[2]);
            return;
        case ScriptState::Kind::Buffer:
            std::memcpy(lua_newbuffer(L, state.bytes.size()), state.bytes.data(), state.bytes.size());
            return;
        case ScriptState::Kind::Table:
            break;
    }
    lua_createtable(L, 0, static_cast<int>(state.pairs.size()));
    for (const auto &entry : state.pairs) {
        PushState(L, entry.first);
        PushState(L, entry.second);
        lua_rawset(L, -3);
    }
}

// rebuilding allocates, so it runs under Pcall and an out of memory stays a
// script error
int RebuildState(lua_State *L) {
    PushState(L, *static_cast<const ScriptState *>(lua_tolightuserdata(L, 1)));
    return 1;
}

// leaves the chunk or its compile error on the stack
bool LoadChunk(lua_State *L, const char *chunkname, const std::string &source) {
    lua_CompileOptions opts = {};
    opts.optimizationLevel = 2;
    opts.debugLevel = 1;
    opts.typeInfoLevel = 1;
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

// host replacement for Luau's require: resolves game/<name>.luau through the
// vfs, runs it once and caches whatever it returned
// the name is canonicalised first so the cache key, the chunk name and the vfs
// path always agree. Without that, require("a/b") and require("a\\b") would read
// the same file but run and cache it twice, giving two modules with two states
int HostRequire(lua_State *L) {
    std::size_t len = 0;
    const char *raw = luaL_checklstring(L, 1, &len);
    std::string module(raw, len);
    for (char &c : module)
        if (c == '\\') c = '/';
    // an embedded NUL truncates the name, and . or .. escapes game/
    if (module.find('\0') != std::string::npos || !vfs::PlainPath(module))
        luaL_error(L, "module '%s' is not a plain game/ path", module.c_str());
    lua_settop(L, 0);
    lua_pushlstring(L, module.data(), module.size());  // 1: canonical name
    const char *name = lua_tostring(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, kModuleCache);  // 2: cache
    lua_rawgetfield(L, 2, name);                       // 3: cached value
    if (lua_islightuserdata(L, 3) && lua_tolightuserdata(L, 3) == &kLoading)
        luaL_error(L, "module '%s' requires itself", name);
    if (!lua_isnil(L, 3)) return 1;
    lua_pop(L, 1);

    std::string path(name);
    path += ".luau";
    std::optional<std::string> source = vfs::ReadScript(path);
    if (!source) luaL_error(L, "module '%s' not found", name);

    std::string chunkname = "=game/";
    chunkname += path;
    if (!LoadChunk(L, chunkname.c_str(), *source)) lua_error(L);  // 3: chunk, or the error

    lua_pushlightuserdata(L, &kLoading);
    lua_rawsetfield(L, 2, name);

    // the traceback handler has to run at the point of the error, before the
    // stack unwinds, so the module gets its own rather than relying on the
    // enclosing pcall to see frames that are already gone
    lua_getfield(L, LUA_REGISTRYINDEX, kTraceback);
    lua_insert(L, 3);  // 3: handler, 4: chunk
    if (lua_pcall(L, 0, 1, 3) != LUA_OK) {
        lua_pushnil(L);
        lua_rawsetfield(L, 2, name);  // a module that raised is not a cycle, let it retry
        lua_remove(L, 3);             // drop the handler, leaving the error on top
        lua_error(L);
    }
    lua_remove(L, 3);

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

    // the definitions file promises these, so a missed registration has to fail
    // here rather than reach a script as a nil field
    lua_getglobal(L, "raylib");
    for (const char *name : kHostFunctions) {
        lua_getfield(L, -1, name);
        const bool installed = lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (!installed) {
            std::snprintf(lastError, sizeof(lastError), "host function %s was not installed", name);
            std::fprintf(stderr, "%s\n", lastError);
            lua_pop(L, 1);
            Close();
            return false;
        }
    }
    lua_pop(L, 1);
    return true;
}

void Script::ResetResult() {
    lastState.reset();
    lastStateError.clear();
    lastNil = true;
    lastTruthy = false;
}

void Script::CaptureResult() {
    lastNil = lua_isnil(L, -1);
    lastTruthy = lua_toboolean(L, -1) != 0;
    if (lua_type(L, -1) == LUA_TTABLE) {
        Snapshotter snapshot;
        ScriptState state;
        std::string path = "state";
        // a throw unwinds mid iteration and leaves keys and values behind
        const int top = lua_gettop(L);
        try {
            if (snapshot.Take(L, -1, state, path, 0))
                lastState = std::move(state);
            else
                lastStateError = std::move(snapshot.error);
        } catch (const std::bad_alloc &) {
            lua_settop(L, top);
            lastStateError = "state is too large";
        }
    }
    lua_pop(L, 1);
}

bool Script::TakeResultState(ScriptState &out) {
    if (!lastState) return false;
    out = std::move(*lastState);
    lastState.reset();
    return true;
}

void Script::CaptureError() {
    const char *message = lua_tostring(L, -1);
    std::snprintf(lastError, sizeof(lastError), "%s", message != nullptr ? message : "unknown error");
    lua_pop(L, 1);
    std::fprintf(stderr, "%s\n", lastError);
}

// every host call boundary. raylib's render scopes must come back the way they
// went in: unwinding to the entry depth keeps a nested require inside an open
// BeginDrawing legal, while a forgotten End is reported on the frame it happens
// rather than corrupting later ones
bool Script::Pcall(int nargs, int nresults) {
    int base = lua_gettop(L) - nargs;  // the function being called
    lua_getfield(L, LUA_REGISTRYINDEX, kTraceback);
    lua_insert(L, base);
    const int scopes = adapt::ScopeDepth();
    int status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (status != LUA_OK) {
        adapt::UnwindScopes(scopes);
        CaptureError();
        return false;
    }
    if (adapt::ScopeDepth() != scopes) {
        std::snprintf(lastError, sizeof(lastError), "returned with raylib scope %s left open",
                      adapt::InnermostScopeName());
        adapt::UnwindScopes(scopes);
        lua_settop(L, base - 1);
        std::fprintf(stderr, "%s\n", lastError);
        return false;
    }
    return true;
}

// Reset() only fails when the allocator does, but the boot path carries on so
// the error screen can say so; every call has to survive a dead vm
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

bool Script::CallGlobalTable(const char *name, const ScriptState &arg, bool *missing) {
    if (missing != nullptr) *missing = false;
    ResetResult();
    if (NoState()) return false;

    lua_pushcfunction(L, RebuildState, "rebuild");
    lua_pushlightuserdata(L, const_cast<ScriptState *>(&arg));
    if (!Pcall(1, 1)) return false;

    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);  // the function slot and the rebuilt state
        if (missing != nullptr) *missing = true;
        std::snprintf(lastError, sizeof(lastError), "%s() is not defined", name);
        return false;
    }
    lua_insert(L, -2);  // move the state above the function
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
    path += ".luau";
    if (!vfs::ReadScript(path)) {
        if (missing != nullptr) *missing = true;
        std::snprintf(lastError, sizeof(lastError), "game/%s not found", path.c_str());
        return false;
    }

    lua_getglobal(L, "require");
    lua_pushstring(L, name);
    return Pcall(1, 1);
}
