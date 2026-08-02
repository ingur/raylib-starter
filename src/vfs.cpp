#include "vfs.hpp"

extern "C" {
#include "raylib.h"
#include "miniz.h"
}

#include "bind/raylib_bind.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

mz_zip_archive zip;
std::string blob;  // backing memory for the mounted zip, must outlive it
bool mounted;

// module names and windows paths use backslashes, the pak and disk do not
std::string Normalize(std::string_view path) {
    std::string name(path);
    for (char &c : name)
        if (c == '\\') c = '/';
    return name;
}

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

// raylib frees these with MemFree, so this is the one place the vfs copies out
// MemAlloc takes an unsigned int, so a larger asset would wrap the allocation
unsigned char *CopyOut(const std::string &data) {
    if (data.size() > static_cast<std::size_t>(UINT_MAX) - 1) return nullptr;
    auto *out = static_cast<unsigned char *>(MemAlloc(static_cast<unsigned int>(data.size() + 1)));
    if (out == nullptr) return nullptr;
    std::memcpy(out, data.data(), data.size());
    out[data.size()] = '\0';
    return out;
}

// save/ is the one directory the web build keeps, everywhere else is the
// user's own filesystem and raylib's rules apply
std::string WritePath(const char *path) {
    std::string name = Normalize(path);
#if defined(__EMSCRIPTEN__)
    // raylib prefixes the working directory, which the browser reports as /
    std::string_view rest(name);
    while (rest.rfind('/', 0) == 0) rest.remove_prefix(1);
    if (rest.rfind("save/", 0) != 0)
        TraceLog(LOG_WARNING, "VFS: [%s] is outside save/, the browser will not keep it", name.c_str());
#endif
    return name;
}

bool WriteFile(const std::string &path, const void *data, std::size_t size) {
    const std::size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && MakeDirectory(path.substr(0, slash).c_str()) != 0)
        return false;
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool written = size == 0 || std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && written;
}

bool Exists(const char *path) {
    const std::string name = Normalize(path);
    if (IsPathFile(name.c_str())) return true;
    if (!mounted) return false;
    const int index = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0) return false;
    mz_zip_archive_file_stat info;
    return mz_zip_reader_file_stat(&zip, index, &info) && !info.m_is_directory;
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

int LuaSaveFileText(lua_State *L) {
    std::size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    lua_pushboolean(L, WriteFile(WritePath(luaL_checkstring(L, 1)), text, len));
    return 1;
}

int LuaSaveFileData(lua_State *L) {
    std::size_t len = 0;
    const void *data = luaL_checkbuffer(L, 2, &len);
    lua_pushboolean(L, WriteFile(WritePath(luaL_checkstring(L, 1)), data, len));
    return 1;
}

// the pak records a time for every entry, so a packed asset has one too
int LuaGetFileModTime(lua_State *L) {
    const std::string name = Normalize(luaL_checkstring(L, 1));
    if (IsPathFile(name.c_str())) {
        lua_pushnumber(L, static_cast<double>(GetFileModTime(name.c_str())));
        return 1;
    }
    mz_zip_archive_file_stat info;
    const int index = mounted
        ? mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE)
        : -1;
    if (index < 0 || !mz_zip_reader_file_stat(&zip, index, &info) || info.m_is_directory)
        luaL_error(L, "[%s] not found", name.c_str());
    lua_pushnumber(L, static_cast<double>(info.m_time));
    return 1;
}

int LuaFileExists(lua_State *L) {
    lua_pushboolean(L, Exists(luaL_checkstring(L, 1)));
    return 1;
}

// raylib's own version measures loose files only, so a packed asset reads as absent
int LuaGetFileLength(lua_State *L) {
    const std::string name = Normalize(luaL_checkstring(L, 1));
    if (IsPathFile(name.c_str())) {
        lua_pushinteger(L, GetFileLength(name.c_str()));
        return 1;
    }
    mz_zip_archive_file_stat info;
    const int index = mounted
        ? mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE)
        : -1;
    if (index < 0 || !mz_zip_reader_file_stat(&zip, index, &info) || info.m_is_directory)
        luaL_error(L, "[%s] not found", name.c_str());
    lua_pushnumber(L, static_cast<double>(info.m_uncomp_size));
    return 1;
}

void PushPaths(lua_State *L, const std::vector<std::string> &paths) {
    lua_createtable(L, static_cast<int>(paths.size()), 0);
    for (std::size_t i = 0; i < paths.size(); i++) {
        lua_pushlstring(L, paths[i].data(), paths[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
}

// a zip need not carry directory entries, so a directory is any prefix some
// file lives under
int LuaDirectoryExists(lua_State *L) {
    std::string dir = Normalize(luaL_checkstring(L, 1));
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (DirectoryExists(dir.c_str())) {
        lua_pushboolean(L, true);
        return 1;
    }
    const std::string prefix = dir + "/";
    for (mz_uint i = 0; mounted && i < mz_zip_reader_get_num_files(&zip); i++) {
        mz_zip_archive_file_stat info;
        if (!mz_zip_reader_file_stat(&zip, i, &info)) continue;
        if (std::strncmp(info.m_filename, prefix.c_str(), prefix.size()) == 0) {
            lua_pushboolean(L, true);
            return 1;
        }
    }
    lua_pushboolean(L, false);
    return 1;
}

bool DeclaresMaterials(const std::string &obj) {
    for (std::size_t line = 0; line < obj.size();) {
        const std::size_t end = obj.find('\n', line);
        const std::size_t stop = end == std::string::npos ? obj.size() : end;
        const std::size_t text = obj.find_first_not_of(" \t", line);
        if (text != std::string::npos && text < stop) {
            const std::string_view rest(obj.data() + text, stop - text);
            if (rest.size() > 7 && rest.compare(0, 6, "mtllib") == 0 && (rest[6] == ' ' || rest[6] == '\t'))
                return true;
        }
        if (end == std::string::npos) break;
        line = end + 1;
    }
    return false;
}

// rmodels.c switches the working directory so tinyobj can open the mtl with
// its own fopen, and leaves it there when the parse fails. neither the mtl nor
// the textures it names reach the vfs callbacks, so a packed obj loses them
int LuaLoadModel(lua_State *L) {
    const char *fileName = luaL_checkstring(L, 1);
    if (IsFileExtension(fileName, ".obj") && !IsPathFile(fileName)) {
        const std::optional<std::string> obj = vfs::Read(fileName);
        if (obj && DeclaresMaterials(*obj))
            TraceLog(LOG_WARNING, "VFS: [%s] declares materials, which stay in the pak. keep the obj, "
                                  "its mtl and their textures as loose files, or use gltf",
                     fileName);
    }

    const std::string cwd = GetWorkingDirectory();
    const Model model = LoadModel(fileName);
    if (cwd != GetWorkingDirectory()) ChangeDirectory(cwd.c_str());
    bind::Conv<Model>::Push(L, model);
    return 1;
}

// raylib returns an owning array, the host copies each entry into its own
// handle and frees the array, so the poses belong to the copies
int LuaLoadModelAnimations(lua_State *L) {
    const char *fileName = luaL_checkstring(L, 1);
    int count = 0;  // the IQM and GLTF paths leave this untouched when they fail
    ModelAnimation *animations = LoadModelAnimations(fileName, &count);
    if (animations == nullptr || count <= 0) luaL_error(L, "[%s] has no animations", fileName);

    try {
        lua_createtable(L, count, 0);
        for (int i = 0; i < count; i++) {
            bind::Conv<ModelAnimation>::Push(L, animations[i]);
            lua_rawseti(L, -2, i + 1);
        }
    } catch (...) {
        UnloadModelAnimations(animations, count);
        throw;
    }
    MemFree(animations);
    return 1;
}

// each handle owns one animation's poses, so a handle is the unit of release
void ReleaseAnimation(lua_State *L, int narg) {
    if (bind::IsReleased(L, narg, bind::UdTraits<ModelAnimation>::kName)) return;
    ModelAnimation *anim = bind::CheckUd<ModelAnimation>(L, narg);
    for (int frame = 0; frame < anim->keyframeCount; frame++) MemFree(anim->keyframePoses[frame]);
    MemFree(anim->keyframePoses);
    *anim = ModelAnimation{};
    lua_setuserdatatag(L, narg, bind::kReleasedTag);
}

// the loader hands back a table, so unloading one takes either shape
int LuaUnloadModelAnimations(lua_State *L) {
    if (!lua_istable(L, 1)) {
        ReleaseAnimation(L, 1);
        return 0;
    }
    const int count = lua_objlen(L, 1);
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, 1, i);
        ReleaseAnimation(L, -1);
        lua_pop(L, 1);
    }
    return 0;
}

// FilePathList holds a char ** no script can reach, so this returns a table
int LuaLoadDirectoryFiles(lua_State *L) {
    std::string dir = Normalize(luaL_checkstring(L, 1));
    const bool recursive = lua_toboolean(L, 2) != 0;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();

    std::vector<std::string> paths;
    if (DirectoryExists(dir.c_str())) {
        // "FILES*" is raylib's files only filter, its default tag adds directories
        FilePathList loose = LoadDirectoryFilesEx(dir.c_str(), "FILES*", recursive);
        // raylib prefixes the dir as given, so "." yields "./x" while the pak yields x
        for (unsigned int i = 0; i < loose.count; i++) {
            const char *path = loose.paths[i];
            if (path[0] == '.' && path[1] == '/') path += 2;
            paths.emplace_back(path);
        }
        UnloadDirectoryFiles(loose);
    }

    const std::string prefix = dir == "." ? std::string() : dir + "/";
    for (mz_uint i = 0; mounted && i < mz_zip_reader_get_num_files(&zip); i++) {
        mz_zip_archive_file_stat info;
        if (!mz_zip_reader_file_stat(&zip, i, &info) || info.m_is_directory) continue;
        const std::string entry(info.m_filename);
        if (entry.compare(0, prefix.size(), prefix) != 0) continue;
        if (recursive || entry.find('/', prefix.size()) == std::string::npos) paths.push_back(entry);
    }

    // a loose file shadows its packed copy, exactly as reading one does
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    PushPaths(L, paths);
    return 1;
}

// dropped paths are absolute and outside the vfs, drop order is the user's
int LuaLoadDroppedFiles(lua_State *L) {
    FilePathList dropped = LoadDroppedFiles();
    std::vector<std::string> paths;
    for (unsigned int i = 0; i < dropped.count; i++) paths.emplace_back(dropped.paths[i]);
    UnloadDroppedFiles(dropped);
    PushPaths(L, paths);
    return 1;
}

// LoadMusicStream opens the path with its own decoder and never consults
// raylib's file callbacks, so packed music cannot reach it. the FromMemory
// variant keeps the caller's pointer, so the host owns those bytes until
// UnloadMusicStream returns
struct MusicBuffer {
    void *ctx;
    unsigned char *bytes;
};

std::vector<MusicBuffer> musicBuffers;

int LuaLoadMusicStream(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::optional<std::string> data = vfs::Read(path);
    if (!data) luaL_error(L, "[%s] not found", path);
    if (data->size() > static_cast<std::size_t>(INT_MAX)) luaL_error(L, "[%s] is too large", path);

    // raylib lowercases the extension, so both loaders take the same names
    const char *suffix = GetFileExtension(path);
    std::string type(suffix != nullptr ? suffix : "");
    for (char &c : type)
        if (c >= 'A' && c <= 'Z') c += 32;

    // every allocation that can fail happens before the buffer exists, so once the
    // decoder borrows it nothing left can throw and lose the pointer
    musicBuffers.reserve(musicBuffers.size() + 1);
    Music *slot = bind::NewUd<Music>(L, Music{});

    unsigned char *bytes = CopyOut(*data);
    if (bytes == nullptr) luaL_error(L, "out of memory reading [%s]", path);

    Music music = LoadMusicStreamFromMemory(type.c_str(), bytes, static_cast<int>(data->size()));
    if (music.ctxData == nullptr) {
        MemFree(bytes);
        luaL_error(L, "[%s] is not a supported music format", path);
    }
    *slot = music;
    musicBuffers.push_back({music.ctxData, bytes});
    return 1;
}

int LuaUnloadMusicStream(lua_State *L) {
    if (bind::IsReleased(L, 1, bind::UdTraits<Music>::kName)) return 0;
    Music *music = bind::CheckUd<Music>(L, 1);
    void *ctx = music->ctxData;

    UnloadMusicStream(*music);
    *music = Music{};
    lua_setuserdatatag(L, 1, bind::kReleasedTag);

    for (auto it = musicBuffers.begin(); it != musicBuffers.end(); ++it) {
        if (it->ctx == ctx) {
            MemFree(it->bytes);
            musicBuffers.erase(it);
            break;
        }
    }
    return 0;
}

}  // namespace

// raylib's callback pointers have C language linkage
extern "C" {

static unsigned char *LoadData(const char *fileName, int *dataSize) {
    *dataSize = 0;
    std::optional<std::string> data = vfs::Read(fileName);
    if (data && data->size() > static_cast<std::size_t>(INT_MAX)) {
        TraceLog(LOG_WARNING, "VFS: [%s] is too large", fileName);
        return nullptr;
    }
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

// raylib routes SaveFileData, SaveFileText, TakeScreenshot and the Export
// functions through these, except bmp and qoi images, which fopen directly
static bool SaveData(const char *fileName, void *data, int dataSize) {
    return dataSize >= 0 && WriteFile(WritePath(fileName), data, static_cast<std::size_t>(dataSize));
}

static bool SaveText(const char *fileName, const char *text) {
    return WriteFile(WritePath(fileName), text, std::strlen(text));
}

}  // extern "C"

namespace vfs {

bool PlainPath(std::string_view path) {
    if (path.empty()) return false;
    for (std::size_t start = 0;;) {
        const std::size_t slash = path.find('/', start);
        const std::string_view segment = path.substr(start, slash - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (slash == std::string_view::npos) return true;
        start = slash + 1;
    }
}

std::optional<std::string> Read(std::string_view path) {
    const std::string name = Normalize(path);
    std::optional<std::string> data = ReadDisk(name.c_str());
    return data ? data : ReadPak(name.c_str());
}

std::optional<std::string> ReadScript(std::string_view path) {
    std::string prefixed = "game/";
    prefixed += path;
    return Read(prefixed);
}

void OpenLoaders(lua_State *L) {
    lua_getglobal(L, "raylib");
    lua_pushcfunction(L, LuaLoadFileText, "LoadFileText");
    lua_setfield(L, -2, "LoadFileText");
    lua_pushcfunction(L, LuaLoadFileData, "LoadFileData");
    lua_setfield(L, -2, "LoadFileData");
    lua_pushcfunction(L, LuaLoadMusicStream, "LoadMusicStream");
    lua_setfield(L, -2, "LoadMusicStream");
    lua_pushcfunction(L, LuaUnloadMusicStream, "UnloadMusicStream");
    lua_setfield(L, -2, "UnloadMusicStream");
    lua_pushcfunction(L, LuaSaveFileText, "SaveFileText");
    lua_setfield(L, -2, "SaveFileText");
    lua_pushcfunction(L, LuaSaveFileData, "SaveFileData");
    lua_setfield(L, -2, "SaveFileData");
    lua_pushcfunction(L, LuaGetFileModTime, "GetFileModTime");
    lua_setfield(L, -2, "GetFileModTime");
    lua_pushcfunction(L, LuaFileExists, "FileExists");
    lua_setfield(L, -2, "FileExists");
    lua_pushcfunction(L, LuaDirectoryExists, "DirectoryExists");
    lua_setfield(L, -2, "DirectoryExists");
    lua_pushcfunction(L, LuaGetFileLength, "GetFileLength");
    lua_setfield(L, -2, "GetFileLength");
    lua_pushcfunction(L, LuaLoadDirectoryFiles, "LoadDirectoryFiles");
    lua_setfield(L, -2, "LoadDirectoryFiles");
    lua_pushcfunction(L, LuaLoadDroppedFiles, "LoadDroppedFiles");
    lua_setfield(L, -2, "LoadDroppedFiles");
    lua_pushcfunction(L, LuaLoadModel, "LoadModel");
    lua_setfield(L, -2, "LoadModel");
    lua_pushcfunction(L, LuaLoadModelAnimations, "LoadModelAnimations");
    lua_setfield(L, -2, "LoadModelAnimations");
    lua_pushcfunction(L, LuaUnloadModelAnimations, "UnloadModelAnimations");
    lua_setfield(L, -2, "UnloadModelAnimations");
    lua_pop(L, 1);
}

bool Mount() {
    SetLoadFileDataCallback(LoadData);
    SetLoadFileTextCallback(LoadText);
    SetSaveFileDataCallback(SaveData);
    SetSaveFileTextCallback(SaveText);

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
    SetSaveFileDataCallback(nullptr);
    SetSaveFileTextCallback(nullptr);
    if (!mounted) return;
    mz_zip_reader_end(&zip);
    std::string().swap(blob);
    mounted = false;
}

}  // namespace vfs

