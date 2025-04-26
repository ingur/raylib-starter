#include <climits>
#include <cstring>
#include <sstream>
#include <vector>
#include <string>
#include <map>

#include "raylib/raylib.h"
#include "luajit/lua.hpp"
#include "miniz/miniz.h"

extern "C"
{
    static std::map<std::string, mz_zip_archive *> g_DataArchives;
    static lua_State *L;

    static unsigned char *FS_LoadFileData(const char *fileName, int *dataSize);
    static char *FS_LoadFileText(const char *fileName);
}

bool InitFS(const char *manifest_path)
{
    auto manifest_content = LoadFileText(manifest_path);
    if (!manifest_content)
    {
        printf("Error: Could not load %s\n", manifest_path);
        return false;
    }

    std::stringstream ss(manifest_content);
    std::string archive_name;
    while (std::getline(ss, archive_name))
    {
        if (archive_name.empty() || archive_name[0] == '#')
            continue;

        mz_zip_archive *archive = new mz_zip_archive();
        memset(archive, 0, sizeof(mz_zip_archive));
        if (!mz_zip_reader_init_file(archive, archive_name.c_str(), 0))
        {
            printf("Error: Could not initialize archive %s\n", archive_name.c_str());
            UnloadFileText(manifest_content);
            mz_zip_reader_end(archive);
            delete archive;
            return false;
        }
        printf("Loaded archive: %s\n", archive_name.c_str());

        std::string base_name = archive_name;
        size_t dot_pos = archive_name.rfind('.');
        if (dot_pos != std::string::npos)
            base_name = archive_name.substr(0, dot_pos);

        size_t slash_pos = base_name.find_first_of("/\\");
        if (slash_pos != std::string::npos)
            base_name = base_name.substr(slash_pos + 1);

        // NOTE: all base_names should be unique in the manifest
        g_DataArchives[base_name] = archive;
    }

    UnloadFileText(manifest_content);

    if (g_DataArchives.empty())
    {
        printf("Error: No archives loaded from %s\n", manifest_path);
        return false;
    }

    SetLoadFileDataCallback(FS_LoadFileData);
    SetLoadFileTextCallback(FS_LoadFileText);

    return true;
}

void UnloadFS()
{
    for (auto &pair : g_DataArchives)
    {
        mz_zip_archive *archive = pair.second;
        mz_zip_reader_end(archive);
        delete archive;
    }
    g_DataArchives.clear();
}

bool RunLuaFiles(std::vector<std::string> fileNames)
{
    for (const auto &fileName : fileNames)
    {
        const char *file_content = FS_LoadFileText(fileName.c_str());
        if (!file_content)
        {
            printf("Error: Could not load %s\n", fileName.c_str());
            return false;
        }

        int status = luaL_dostring(L, file_content);
        MemFree((void *)file_content);

        if (status != LUA_OK)
        {
            const char *error_msg = lua_tostring(L, -1);
            printf("Error running Lua script (%s): %s\n", fileName.c_str(), error_msg);
            lua_pop(L, 1); // Remove the error message from the stack
            return false;
        }
    }

    return true;
}

int main()
{
    // initialize virtual file system
    if (!InitFS("data.manifest"))
    {
        printf("Failed to initialize the file system\n");
        UnloadFS();
        return 1;
    }

    // initialize LuaJIT
    L = luaL_newstate();
    luaL_openlibs(L);

    // run lua scripts
    RunLuaFiles({"lua/raylib.lua", "lua/main.lua"});

    // cleanup
    lua_close(L);
    UnloadFS();

    return 0;
}

extern "C" unsigned char *FS_LoadFileData(const char *filePath, int *dataSize)
{
    if (!filePath || !dataSize)
        return NULL;
    *dataSize = 0;

    // NOTE: doesn't support multi-archive paths yet
    std::string sFilePath(filePath);
    size_t pos = sFilePath.find('/');
    if (pos == std::string::npos || pos == 0 || pos == sFilePath.length() - 1)
    {
        printf("WARN: VFS: Invalid file path format (must be key/path): %s\n", filePath);
        return NULL;
    }
    auto archive_name = sFilePath.substr(0, pos);

    auto archive_it = g_DataArchives.find(archive_name);
    if (archive_it == g_DataArchives.end())
    {
        printf("Warn: Archive %s not found\n", archive_name.c_str());
        return NULL;
    }

    mz_zip_archive *archive = archive_it->second;

    int file_index = mz_zip_reader_locate_file(archive, filePath, NULL, 0);
    if (file_index == -1)
    {
        printf("Warn: File %s not found in archive %s\n", filePath, archive_name.c_str());
        return NULL;
    }

    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(archive, file_index, &file_stat))
    {
        printf("Error: Could not get file stat for %s in archive %s\n", filePath, archive_name.c_str());
        return NULL;
    }

    unsigned char *file_data = (unsigned char *)MemAlloc(file_stat.m_uncomp_size);
    if (!file_data)
    {
        printf("Error: Could not allocate memory for file %s from archive %s\n", filePath, archive_name.c_str());
        return NULL;
    }

    if (!mz_zip_reader_extract_to_mem(archive, file_index, file_data, file_stat.m_uncomp_size, 0))
    {
        printf("Error: Could not extract file %s from archive %s\n", filePath, archive_name.c_str());
        MemFree(file_data);
        return NULL;
    }

    // NOTE: reported dataSize might overflow if the file size is larger than INT_MAX
    if (file_stat.m_uncomp_size > INT_MAX)
        *dataSize = INT_MAX;
    else
        *dataSize = static_cast<int>(file_stat.m_uncomp_size);

    return file_data;
}

extern "C" char *FS_LoadFileText(const char *filePath)
{
    if (!filePath)
        return NULL;

    int dataSize;
    unsigned char *data = FS_LoadFileData(filePath, &dataSize);
    if (!data)
    {
        printf("Warn: Could not load file %s\n", filePath);
        return NULL;
    }

    char *text = (char *)MemAlloc(dataSize + 1);
    if (!text)
    {
        printf("Error: Could not allocate memory for text from file %s\n", filePath);
        MemFree(data);
        return NULL;
    }

    memcpy(text, data, dataSize);
    text[dataSize] = '\0';
    MemFree(data);

    return text;
}
