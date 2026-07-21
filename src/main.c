#include "raylib.h"
#include "pocketpy.h"
#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#include <sys/stat.h>
#endif

void py__add_module_raylib(void);
void py__add_raylib_colors(void);

typedef struct {
    char title[128];
    int width, height, fps;
    unsigned int flags;
} WindowConfig;

static WindowConfig config;
static char lastError[2048];
static py_Name updateName;
static bool scriptOk;
static bool booted;
static bool quit;
static bool devMode;
static bool watch;
static char *reloadState;

static void CaptureError(void) {
    char *text = py_formatexc();
    snprintf(lastError, sizeof(lastError), "%s", text ? text : "unknown error");
    if (text) PK_FREE(text);
    py_clearexc(NULL);
    fprintf(stderr, "%s\n", lastError);
}

static int CfgInt(py_GlobalRef mod, const char *key, int fallback) {
    py_ItemRef value = py_getdict(mod, py_name(key));
    return value && py_isint(value) ? (int)py_toint(value) : fallback;
}

static bool CfgFlag(py_GlobalRef mod, const char *key, bool fallback) {
    py_ItemRef value = py_getdict(mod, py_name(key));
    return value && py_isbool(value) ? py_tobool(value) : fallback;
}

static void LoadWindowConfig(void) {
    // DEV is a builtin, usable from any script including window.py
    py_newbool(py_emplacedict(py_getmodule("builtins"), py_name("DEV")), devMode);

    snprintf(config.title, sizeof(config.title), "game");
    config.width = 960;
    config.height = 540;
    config.fps = 60;
    config.flags = FLAG_VSYNC_HINT;

    char *source = ImportFile("window.py", NULL);
    if (source == NULL) return;

    py_GlobalRef mod = py_newmodule("window");
    bool ok = py_exec(source, "game/window.py", EXEC_MODE, mod);
    PK_FREE(source);
    if (!ok) {
        CaptureError();
        TraceLog(LOG_WARNING, "SCRIPT: game/window.py failed, using defaults");
        return;
    }

    py_ItemRef title = py_getdict(mod, py_name("title"));
    if (title && py_isstr(title)) snprintf(config.title, sizeof(config.title), "%s", py_tostr(title));

    py_ItemRef size = py_getdict(mod, py_name("size"));
    if (size && py_istuple(size) && py_tuple_len(size) == 2) {
        py_ItemRef w = py_tuple_getitem(size, 0);
        py_ItemRef h = py_tuple_getitem(size, 1);
        if (py_isint(w) && py_isint(h)) {
            py_i64 width = py_toint(w);
            py_i64 height = py_toint(h);
            if (width > 0 && height > 0 && width <= 16384 && height <= 16384) {
                config.width = (int)width;
                config.height = (int)height;
            }
        }
    }

    config.fps = CfgInt(mod, "fps", config.fps);
    config.flags = (CfgFlag(mod, "vsync", true) ? FLAG_VSYNC_HINT : 0)
                 | (CfgFlag(mod, "resizable", false) ? FLAG_WINDOW_RESIZABLE : 0)
                 | (CfgFlag(mod, "msaa", false) ? FLAG_MSAA_4X_HINT : 0)
                 | (CfgFlag(mod, "fullscreen", false) ? FLAG_FULLSCREEN_MODE : 0);
}

static bool Boot(void) {
    py_callbacks()->importfile = ImportFile;
    py__add_module_raylib();
    py__add_raylib_colors();
    BindVfsLoaders();
    updateName = py_name("update");

    char *source = ImportFile("main.py", NULL);
    if (source == NULL) {
        snprintf(lastError, sizeof(lastError), "game/main.py not found");
        TraceLog(LOG_ERROR, "SCRIPT: game/main.py not found");
        return false;
    }
    bool ok = py_exec(source, "game/main.py", EXEC_MODE, NULL);
    PK_FREE(source);
    if (!ok) CaptureError();
    return ok;
}

static void Reload(void) {
    if (booted) {
        py_ItemRef before = py_getglobal(py_name("before_reload"));
        if (before != NULL) {
            if (py_call(before, 0, NULL)) {
                if (py_isstr(py_retval())) {
                    if (reloadState == NULL) {  // keep retained state until it applies
                        const char *state = py_tostr(py_retval());
                        size_t len = strlen(state) + 1;
                        reloadState = malloc(len);
                        if (reloadState) memcpy(reloadState, state, len);
                    }
                } else if (!py_isnone(py_retval())) {
                    TraceLog(LOG_WARNING, "SCRIPT: before_reload state must be a string, ignored");
                }
            } else {
                py_clearexc(NULL);
                TraceLog(LOG_WARNING, "SCRIPT: before_reload failed");
            }
        }
    }

    py_resetvm();
    LoadWindowConfig();
    SetWindowTitle(config.title);
    if (!(config.flags & FLAG_FULLSCREEN_MODE)) SetWindowSize(config.width, config.height);
    SetTargetFPS(config.fps);
    scriptOk = booted = Boot();

    if (scriptOk && reloadState != NULL) {
        py_ItemRef after = py_getglobal(py_name("after_reload"));
        if (after != NULL) {
            py_newstr(py_r0(), reloadState);
            if (py_call(after, 1, py_r0())) {
                free(reloadState);
                reloadState = NULL;
            } else {
                CaptureError();
                scriptOk = false;  // state is kept for the next successful boot
            }
        } else {
            free(reloadState);
            reloadState = NULL;
        }
    }
    TraceLog(LOG_INFO, scriptOk ? "SCRIPT: reloaded" : "SCRIPT: reload failed");
}

static bool RunUpdate(void) {
    py_ItemRef update = py_getglobal(updateName);
    if (update == NULL) {
        snprintf(lastError, sizeof(lastError), "update() is not defined in game/main.py");
        TraceLog(LOG_ERROR, "SCRIPT: update() is not defined");
        return false;
    }
    if (!py_call(update, 0, NULL)) {
        CaptureError();
        EndDrawing();  // close the frame if the error hit mid draw
        return false;
    }
    py_Ref result = py_retval();
    if (py_isbool(result) && py_tobool(result)) quit = true;
    return true;
}

// error screen layout, +2 matches raylib's default line spacing
enum { ERR_PAD = 40, ERR_FONT = 20, ERR_LINE = ERR_FONT + 2 };

static char wrappedError[4096];

// copy lastError into wrappedError, breaking lines that exceed maxWidth
static void WrapError(int maxWidth) {
    char *dst = wrappedError, *lineStart = wrappedError;
    char *end = wrappedError + sizeof(wrappedError) - 3;
    for (const char *src = lastError; *src && dst < end; src++) {
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

static void ErrorFrame(void) {
    int width = GetScreenWidth() - 2 * ERR_PAD;
    WrapError(width > 0 ? width : 1);

    int lines = 1;
    for (const char *p = wrappedError; *p; p++) lines += *p == '\n';

    int top = ERR_PAD + 2 * ERR_LINE;
    int bottom = GetScreenHeight() - ERR_PAD - ERR_LINE;
    int y = top;
    if (top + lines * ERR_LINE > bottom) y = bottom - lines * ERR_LINE;  // keep the last lines visible

    BeginDrawing();
    ClearBackground((Color){24, 24, 24, 255});
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

static void Frame(void) {
    if (scriptOk) scriptOk = RunUpdate();
    else ErrorFrame();
#if defined(__EMSCRIPTEN__)
    if (quit) emscripten_cancel_main_loop();
#endif
}

#if !defined(__EMSCRIPTEN__)
// avalanche mixer
static uint64_t Mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}

// fingerprint each file as hash(path, size, mtime)
static void StampDir(const char *dir, const char *filter, uint64_t *stamp, unsigned int *count) {
    if (!DirectoryExists(dir)) return;
    FilePathList files = LoadDirectoryFilesEx(dir, filter, true);
    for (unsigned int i = 0; i < files.count; i++) {
        struct stat info;
        if (stat(files.paths[i], &info) != 0) continue;
        uint64_t h = 5381;
        for (const char *p = files.paths[i]; *p; p++) h = (h * 33) ^ (unsigned char)*p;
        h = (h * 33) ^ (uint64_t)info.st_size;
        h = (h * 33) ^ (uint64_t)info.st_mtime;
#if defined(__linux__)
        h = (h * 33) ^ (uint64_t)info.st_mtim.tv_nsec;  // include subsecond mtimes
#endif
        *stamp += Mix64(h);
        *count += 1;
    }
    UnloadDirectoryFiles(files);
}

static void WatchFiles(void) {
    static double next;
    static uint64_t seen;
    static unsigned int seenCount;
    static bool initialized;
    double now = GetTime();
    if (now < next) return;
    next = now + 0.5;

    uint64_t stamp = 0;
    unsigned int count = 0;
    StampDir("game", ".py", &stamp, &count);
    StampDir("assets", NULL, &stamp, &count);

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

int main(void) {
#if defined(NDEBUG) && !defined(__EMSCRIPTEN__)
    // release: resolve the pak, save/ and loose files next to the executable
    ChangeDirectory(GetApplicationDirectory());
#endif
    MountAssets();
#if !defined(__EMSCRIPTEN__)
    MakeDirectory("save");  // writable save dir, on web an IDBFS mount (src/web_save.js)
#endif
#if !defined(NDEBUG)
    devMode = true;  // debug builds get the DEV builtin
#endif
#if !defined(__EMSCRIPTEN__)
    const char *w = getenv("WATCH");  // ./build.sh dev sets WATCH=1
    watch = devMode && w != NULL && strcmp(w, "1") == 0;
#endif
    py_initialize();
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

    py_finalize();
    CloseWindow();
    UnmountAssets();
    return 0;
}
