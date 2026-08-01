extern "C" {
#include "raylib.h"
}

#include "script.hpp"
#include "vfs.hpp"

#include "lua.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#include <sys/stat.h>
#endif

namespace {

struct WindowConfig {
    char title[128];
    int width, height, fps;
    unsigned int flags;
};

WindowConfig config;
Script script;
bool scriptOk;
bool booted;
bool quit;
bool devMode;
bool watch;
std::string reloadState;
bool retained;  // reloadState is waiting for an after_reload that accepts it

int CfgInt(lua_State *L, int table, const char *key, int fallback) {
    lua_getfield(L, table, key);
    int value = lua_type(L, -1) == LUA_TNUMBER ? (int)lua_tointeger(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

bool CfgFlag(lua_State *L, int table, const char *key, bool fallback) {
    lua_getfield(L, table, key);
    bool value = lua_isboolean(L, -1) ? lua_toboolean(L, -1) != 0 : fallback;
    lua_pop(L, 1);
    return value;
}

// Boots the VM and pulls game/window.lua in through require, so the table read
// here is the very same one the game later gets from require("window").
void LoadWindowConfig() {
    std::snprintf(config.title, sizeof(config.title), "game");
    config.width = 960;
    config.height = 540;
    config.fps = 60;
    config.flags = FLAG_VSYNC_HINT;

    if (!script.Reset(devMode)) {
        TraceLog(LOG_ERROR, "SCRIPT: %s", script.Error());
        return;
    }

    bool missing = false;
    if (!script.Require("window", &missing)) {
        if (!missing) TraceLog(LOG_WARNING, "SCRIPT: game/window.lua failed, using defaults");
        return;
    }

    lua_State *L = script.State();
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        TraceLog(LOG_WARNING, "SCRIPT: game/window.lua must return a table, using defaults");
        return;
    }
    int table = lua_gettop(L);

    lua_getfield(L, table, "title");
    if (lua_type(L, -1) == LUA_TSTRING)
        std::snprintf(config.title, sizeof(config.title), "%s", lua_tostring(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, table, "size");
    if (lua_istable(L, -1) && lua_objlen(L, -1) == 2) {
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        if (lua_type(L, -2) == LUA_TNUMBER && lua_type(L, -1) == LUA_TNUMBER) {
            int width = (int)lua_tointeger(L, -2);
            int height = (int)lua_tointeger(L, -1);
            if (width > 0 && height > 0 && width <= 16384 && height <= 16384) {
                config.width = width;
                config.height = height;
            }
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 1);

    config.fps = CfgInt(L, table, "fps", config.fps);
    config.flags = (CfgFlag(L, table, "vsync", true) ? (unsigned int)FLAG_VSYNC_HINT : 0u)
                 | (CfgFlag(L, table, "resizable", false) ? (unsigned int)FLAG_WINDOW_RESIZABLE : 0u)
                 | (CfgFlag(L, table, "msaa", false) ? (unsigned int)FLAG_MSAA_4X_HINT : 0u)
                 | (CfgFlag(L, table, "fullscreen", false) ? (unsigned int)FLAG_FULLSCREEN_MODE : 0u);

    lua_pop(L, 1);  // the config table
}

bool Boot() {
    bool missing = false;
    if (!script.RunEntry("main.lua", &missing)) {
        if (missing) TraceLog(LOG_ERROR, "SCRIPT: game/main.lua not found");
        return false;
    }
    return true;
}

void Reload() {
    if (booted) {
        bool missing = false;
        if (script.CallGlobal("before_reload", &missing)) {
            const char *state = script.LastResultString();
            if (state != nullptr) {
                if (!retained) {  // keep retained state until it applies
                    reloadState = state;
                    retained = true;
                }
            } else if (!script.LastResultNil()) {
                TraceLog(LOG_WARNING, "SCRIPT: before_reload state must be a string, ignored");
            }
        } else if (!missing) {
            TraceLog(LOG_WARNING, "SCRIPT: before_reload failed");
        }
    }

    LoadWindowConfig();  // also resets the vm
    SetWindowTitle(config.title);
    if (!(config.flags & FLAG_FULLSCREEN_MODE)) SetWindowSize(config.width, config.height);
    SetTargetFPS(config.fps);
    scriptOk = booted = Boot();

    if (scriptOk && retained) {
        bool missing = false;
        if (script.CallGlobalStr("after_reload", reloadState.c_str(), &missing) || missing) {
            reloadState.clear();
            retained = false;
        } else {
            scriptOk = false;  // state is kept for the next successful boot
        }
    }
    TraceLog(LOG_INFO, scriptOk ? "SCRIPT: reloaded" : "SCRIPT: reload failed");
}

bool RunUpdate() {
    bool missing = false;
    if (!script.CallGlobal("update", &missing)) {
        if (missing) TraceLog(LOG_ERROR, "SCRIPT: update() is not defined");
        else EndDrawing();  // close the frame if the error hit mid draw
        return false;
    }
    if (script.LastResultTruthy()) quit = true;
    return true;
}

// error screen layout, +2 matches raylib's default line spacing
enum { ERR_PAD = 40, ERR_FONT = 20, ERR_LINE = ERR_FONT + 2 };

char wrappedError[4096];

// copy the script error into wrappedError, breaking lines that exceed maxWidth
void WrapError(int maxWidth) {
    char *dst = wrappedError, *lineStart = wrappedError;
    char *end = wrappedError + sizeof(wrappedError) - 3;
    for (const char *src = script.Error(); *src && dst < end; src++) {
        *dst = (*src == '\t') ? ' ' : *src;
        if (*dst == '\n') { dst++; lineStart = dst; continue; }
        dst[1] = '\0';
        if (dst > lineStart && MeasureText(lineStart, ERR_FONT) > maxWidth) {
            char c = *dst;
            *dst++ = '\n';
            *dst = c;
            lineStart = dst;
        }
        dst++;
    }
    *dst = '\0';
}

void ErrorFrame() {
    int width = GetScreenWidth() - 2 * ERR_PAD;
    WrapError(width > 0 ? width : 1);

    int lines = 1;
    for (const char *p = wrappedError; *p; p++) lines += *p == '\n';

    int top = ERR_PAD + 2 * ERR_LINE;
    int bottom = GetScreenHeight() - ERR_PAD - ERR_LINE;
    int y = top;
    if (top + lines * ERR_LINE > bottom) y = bottom - lines * ERR_LINE;  // keep the last lines visible

    BeginDrawing();
    ClearBackground(Color{24, 24, 24, 255});
    DrawText("script error", ERR_PAD, ERR_PAD, ERR_FONT + 10, RED);
    BeginScissorMode(0, top, GetScreenWidth(), bottom - top);
    DrawText(wrappedError, ERR_PAD, y, ERR_FONT, RAYWHITE);
    EndScissorMode();
    if (devMode)
        DrawText(watch ? "save a file or press F5 to reload" : "press F5 to reload",
                 ERR_PAD, GetScreenHeight() - ERR_PAD, ERR_FONT, GRAY);
    EndDrawing();
    if (devMode && IsKeyPressed(KEY_F5)) Reload();
}

#if !defined(__EMSCRIPTEN__)
// avalanche mixer
std::uint64_t Mix64(std::uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}

// fingerprint each file as hash(path, size, mtime)
void StampDir(const char *dir, const char *filter, std::uint64_t *stamp, unsigned int *count) {
    if (!DirectoryExists(dir)) return;
    FilePathList files = LoadDirectoryFilesEx(dir, filter, true);
    for (unsigned int i = 0; i < files.count; i++) {
        struct stat info;
        if (::stat(files.paths[i], &info) != 0) continue;
        std::uint64_t h = 5381;
        for (const char *p = files.paths[i]; *p; p++) h = (h * 33) ^ (unsigned char)*p;
        h = (h * 33) ^ (std::uint64_t)info.st_size;
        h = (h * 33) ^ (std::uint64_t)info.st_mtime;
#if defined(__linux__)
        h = (h * 33) ^ (std::uint64_t)info.st_mtim.tv_nsec;  // include subsecond mtimes
#endif
        *stamp += Mix64(h);
        *count += 1;
    }
    UnloadDirectoryFiles(files);
}

void WatchFiles() {
    static double next;
    static std::uint64_t seen;
    static unsigned int seenCount;
    static bool initialized;
    double now = GetTime();
    if (now < next) return;
    next = now + 0.5;

    std::uint64_t stamp = 0;
    unsigned int count = 0;
    StampDir("game", ".lua", &stamp, &count);
    StampDir("assets", nullptr, &stamp, &count);

    if (!initialized) {
        seen = stamp;
        seenCount = count;
        initialized = true;
        return;
    }
    if (stamp != seen || count != seenCount) {
        seen = stamp;
        seenCount = count;
        Reload();
    }
}
#endif

}  // namespace

// emscripten's main loop callback pointer has C language linkage
extern "C" {

static void Frame(void) {
    if (scriptOk) scriptOk = RunUpdate();
    else ErrorFrame();
#if defined(__EMSCRIPTEN__)
    if (quit) emscripten_cancel_main_loop();
#endif
}

}  // extern "C"

int main(void) {
#if defined(NDEBUG) && !defined(__EMSCRIPTEN__)
    // release: resolve the pak, save/ and loose files next to the executable
    ChangeDirectory(GetApplicationDirectory());
#endif
    vfs::Mount();
#if !defined(__EMSCRIPTEN__)
    MakeDirectory("save");  // writable save dir, on web an IDBFS mount (src/web_save.js)
#endif
#if !defined(NDEBUG)
    devMode = true;  // debug builds get the DEV global
#endif
#if !defined(__EMSCRIPTEN__)
    const char *w = std::getenv("WATCH");  // ./build.sh dev sets WATCH=1
    watch = devMode && w != nullptr && std::strcmp(w, "1") == 0;
#endif
    LoadWindowConfig();

    SetConfigFlags(config.flags);
    InitWindow(config.width, config.height, config.title);
    if (!IsWindowReady()) {
        TraceLog(LOG_ERROR, "WINDOW: failed to initialize");
        return 1;
    }
    SetTargetFPS(config.fps);

    scriptOk = booted = Boot();

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop(Frame, 0, 1);
#else
    while (!WindowShouldClose() && !quit) {
        if (watch) WatchFiles();
        Frame();
    }
#endif

    script.Close();
    CloseWindow();
    vfs::Unmount();
    return 0;
}
