#include <vector>
#include <string>

#include <raylib/raylib.h>
#include <luajit/lua.hpp>

#include "filesystem.hpp"

static lua_State *L;

static bool RunLuaFiles(const std::vector<std::string> &luaFiles);

int main()
{
    // Initialize virtual file system
    if (!InitVFS("data.manifest"))
    {
        TraceLog(LOG_ERROR, "MAIN: Failed to initialize the file system");
        UnloadVFS();
        return 1;
    }

    // Initialize LuaJIT
    L = luaL_newstate();
    luaL_openlibs(L);

    // Run Lua scripts
    RunLuaFiles({"lua/raylib.lua", "lua/main.lua"});

    // Cleanup
    lua_close(L);
    UnloadVFS();

    return 0;
}

static bool RunLuaFiles(const std::vector<std::string> &luaFiles)
{
    if (!L)
    {
        TraceLog(LOG_ERROR, "LUA: Lua state is not initialized.");
        return false;
    }

    for (const auto &fileName : luaFiles)
    {
        const char *file_content = LoadFileText(fileName.c_str());
        if (!file_content)
        {
            TraceLog(LOG_ERROR, "LUA: Could not load Lua file %s", fileName.c_str());
            return false;
        }

        int status = luaL_dostring(L, file_content);
        MemFree((void *)file_content);

        if (status != LUA_OK)
        {
            const char *error_msg = lua_tostring(L, -1);
            TraceLog(LOG_ERROR, "LUA: Error running Lua script (%s): %s", fileName.c_str(), error_msg ? error_msg : "Unknown error");
            lua_pop(L, 1);
            return false;
        }
    }

    return true;
}
