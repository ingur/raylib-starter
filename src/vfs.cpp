#include "vfs.hpp"

extern "C" {
#include "raylib.h"
#include "miniz.h"
}

#include "lua.h"
#include "lualib.h"

#include <cstdio>
#include <cstring>

namespace {

mz_zip_archive zip;
std::string blob;  // backing memory for the mounted zip, must outlive it
bool mounted;

std::optional<std::string> ReadDisk(const char *path) {
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) return std::nullopt;
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 0) {  // empty files are valid, only reject ftell errors
        std::fclose(f);
        return std::nullopt;
    }

    std::string data(static_cast<size_t>(len), '\0');
    size_t read = len > 0 ? std::fread(&data[0], 1, static_cast<size_t>(len), f) : 0;
    std::fclose(f);
    if (read != static_cast<size_t>(len)) return std::nullopt;
    return data;
}

std::optional<std::string> ReadPak(const char *path) {
    if (!mounted) return std::nullopt;
    int index = mz_zip_reader_locate_file(&zip, path, nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0) return std::nullopt;

    mz_zip_archive_file_stat info;
    if (!mz_zip_reader_file_stat(&zip, index, &info)) return std::nullopt;

    std::string data(static_cast<size_t>(info.m_uncomp_size), '\0');
    if (!data.empty() && !mz_zip_reader_extract_to_mem(&zip, index, &data[0], data.size(), 0))
        return std::nullopt;
    return data;
}

// raylib owns whatever these hand back and releases it with MemFree, so this is
// the one place the vfs copies out of its own std::string.
unsigned char *CopyOut(const std::string &data) {
    auto *out = static_cast<unsigned char *>(MemAlloc(static_cast<unsigned int>(data.size() + 1)));
    if (out == nullptr) return nullptr;
    std::memcpy(out, data.data(), data.size());
    out[data.size()] = '\0';
    return out;
}

int LuaLoadFileText(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::optional<std::string> data = vfs::Read(path);
    if (!data) luaL_error(L, "[%s] not found", path);
    lua_pushlstring(L, data->data(), data->size());
    return 1;
}

int LuaLoadFileData(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::optional<std::string> data = vfs::Read(path);
    if (!data) luaL_error(L, "[%s] not found", path);
    std::memcpy(lua_newbuffer(L, data->size()), data->data(), data->size());
    return 1;
}

}  // namespace

// raylib's callback pointers have C language linkage
extern "C" {

static unsigned char *LoadData(const char *fileName, int *dataSize) {
    *dataSize = 0;
    std::optional<std::string> data = vfs::Read(fileName);
    if (!data) {
        TraceLog(LOG_WARNING, "VFS: [%s] not found", fileName);
        return nullptr;
    }
    unsigned char *out = CopyOut(*data);
    if (out != nullptr) *dataSize = static_cast<int>(data->size());
    return out;
}

static char *LoadText(const char *fileName) {
    std::optional<std::string> data = vfs::Read(fileName);
    if (!data) return nullptr;
    return reinterpret_cast<char *>(CopyOut(*data));
}

}  // extern "C"

namespace vfs {

std::optional<std::string> Read(std::string_view path) {
    std::string name(path);
    std::optional<std::string> data = ReadDisk(name.c_str());
    return data ? data : ReadPak(name.c_str());
}

std::optional<std::string> ReadScript(std::string_view path) {
    std::string prefixed = "game/";
    prefixed += path;
    std::optional<std::string> data = Read(prefixed);
    return data ? data : Read(path);
}

void OpenLoaders(lua_State *L) {
    lua_getglobal(L, "raylib");
    lua_pushcfunction(L, LuaLoadFileText, "LoadFileText");
    lua_setfield(L, -2, "LoadFileText");
    lua_pushcfunction(L, LuaLoadFileData, "LoadFileData");
    lua_setfield(L, -2, "LoadFileData");
    lua_pop(L, 1);
}

bool Mount() {
    SetLoadFileDataCallback(LoadData);
    SetLoadFileTextCallback(LoadText);

#if defined(__EMSCRIPTEN__)
    const char *path = "/" ASSETS_PAK;
#else
    const char *path = TextFormat("%s%s", GetApplicationDirectory(), ASSETS_PAK);
#endif

    std::optional<std::string> data = ReadDisk(path);
    if (!data) {
        TraceLog(LOG_INFO, "VFS: %s not found, using loose files", path);
        return false;
    }
    blob = std::move(*data);

    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, blob.data(), blob.size(), 0)) {
        TraceLog(LOG_WARNING, "VFS: %s is not a valid pak", path);
        std::string().swap(blob);
        return false;
    }

    mounted = true;
    TraceLog(LOG_INFO, "VFS: mounted %s (%d files)", path, (int)mz_zip_reader_get_num_files(&zip));
    return true;
}

void Unmount() {
    SetLoadFileDataCallback(nullptr);
    SetLoadFileTextCallback(nullptr);
    if (!mounted) return;
    mz_zip_reader_end(&zip);
    std::string().swap(blob);
    mounted = false;
}

}  // namespace vfs
