#include "filesystem.hpp"

#include <string_view>
#include <cassert>
#include <climits>
#include <cstring>
#include <sstream>
#include <cstdio>
#include <cerrno>
#include <vector>
#include <string>
#include <memory>
#include <map>

#include <miniz/miniz.h>
#include <raylib/raylib.h>

struct ArchiveInfo
{
    std::unique_ptr<mz_zip_archive> reader;
    std::string fullPath;
};

static std::map<std::string, ArchiveInfo> g_DataArchives;

extern "C"
{
    static unsigned char *LoadFileDataImpl(const char *filePath, int *dataSize);
    static char *LoadFileTextImpl(const char *filePath);
    static bool SaveFileDataImpl(const char *filePath, void *data, int dataSize);
    static bool SaveFileTextImpl(const char *filePath, char *text);
}

bool InitVFS(const char *manifest_path)
{
    char *const manifest_content = LoadFileText(manifest_path);
    if (!manifest_content)
    {
        TraceLog(LOG_ERROR, "VFS: Could not load %s", manifest_path);
        return false;
    }

    std::stringstream ss(manifest_content);
    std::string archive_path;
    while (std::getline(ss, archive_path))
    {
        if (archive_path.empty() || archive_path[0] == '#')
            continue;

        std::string base_name = archive_path;

        const size_t dot_pos = archive_path.rfind('.');
        if (dot_pos != std::string::npos)
            base_name = base_name.substr(0, dot_pos);

        const size_t slash_pos = base_name.find_last_of("/\\");
        if (slash_pos != std::string::npos)
            base_name = base_name.substr(slash_pos + 1);

        if (base_name.empty())
        {
            TraceLog(LOG_ERROR, "VFS: Cannot derive base name from archive path: %s", archive_path.c_str());
            UnloadFileText(manifest_content);
            return false;
        }

        if (g_DataArchives.count(base_name) > 0)
        {
            TraceLog(LOG_ERROR, "VFS: Duplicate archive name found in manifest: %s", base_name.c_str());
            UnloadFileText(manifest_content);
            return false;
        }

        auto archive = std::make_unique<mz_zip_archive>();
        memset(archive.get(), 0, sizeof(mz_zip_archive));
        if (!mz_zip_reader_init_file(archive.get(), archive_path.c_str(), 0))
        {
            TraceLog(LOG_ERROR, "VFS: Could not initialize archive %s", archive_path.c_str());
            UnloadFileText(manifest_content);
            mz_zip_reader_end(archive.get());
            return false;
        }

        TraceLog(LOG_INFO, "VFS: Loaded archive: %s (Key: %s)", archive_path.c_str(), base_name.c_str());
        g_DataArchives[base_name] = {std::move(archive), archive_path};
    }

    UnloadFileText(manifest_content);

    if (g_DataArchives.empty())
    {
        TraceLog(LOG_ERROR, "VFS: No valid archives loaded from manifest: %s", manifest_path);
        return false;
    }

    SetLoadFileDataCallback(LoadFileDataImpl);
    SetLoadFileTextCallback(LoadFileTextImpl);
    SetSaveFileDataCallback(SaveFileDataImpl);
    SetSaveFileTextCallback(SaveFileTextImpl);

    return true;
}

void UnloadVFS()
{
    for (auto &[_, info] : g_DataArchives)
    {
        if (info.reader)
        {
            mz_zip_reader_end(info.reader.get());
        }
    }

    g_DataArchives.clear();

    SetLoadFileDataCallback(nullptr);
    SetLoadFileTextCallback(nullptr);
    SetSaveFileDataCallback(nullptr);
    SetSaveFileTextCallback(nullptr);
}

static std::string GetArchiveKeyFromPath(const char *filePath)
{
    assert(filePath);

    const std::string_view sv(filePath);
    const size_t pos = sv.find('/');
    if (pos == std::string_view::npos || pos == 0 || pos == sv.size() - 1)
        return {};

    return {sv.data(), pos};
}

static unsigned char *FS_LoadFileData(const char *fileName, int &dataSize);
static unsigned char *VFS_LoadFileData(const std::string &archiveKey, const char *filePath, int &dataSize);

extern "C" unsigned char *LoadFileDataImpl(const char *filePath, int *dataSize)
{
    if (!filePath || !dataSize)
        return nullptr;

    const std::string archiveKey = GetArchiveKeyFromPath(filePath);

    return archiveKey.empty()
               ? FS_LoadFileData(filePath, *dataSize)
               : VFS_LoadFileData(archiveKey, filePath, *dataSize);
}

extern "C" char *LoadFileTextImpl(const char *filePath)
{
    if (!filePath)
        return nullptr;

    int dataSize = 0;
    unsigned char *data = LoadFileDataImpl(filePath, &dataSize);

    if (!data)
        return nullptr;

    char *text = reinterpret_cast<char *>(data);
    text[dataSize] = '\0';

    return text;
}

static unsigned char *VFS_LoadFileData(const std::string &archiveKey, const char *filePath, int &dataSize)
{
    assert(!archiveKey.empty());
    assert(filePath);

    auto it = g_DataArchives.find(archiveKey);
    if (it == g_DataArchives.end())
    {
        TraceLog(LOG_WARNING, "VFS: Archive key '%s' not found for loading file: %s", archiveKey.data(), filePath);
        return nullptr;
    }

    const ArchiveInfo &archiveInfo = it->second;
    auto *archiveReader = archiveInfo.reader.get();

    if (!archiveReader || archiveReader->m_zip_mode == MZ_ZIP_MODE_INVALID)
    {
        TraceLog(LOG_WARNING, "VFS: Archive reader for key '%s' is not valid. Cannot load file: %s", archiveKey.data(), filePath);
        return nullptr;
    }

    const int fileIndex = mz_zip_reader_locate_file(archiveReader, filePath, nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (fileIndex < 0)
    {
        TraceLog(LOG_WARNING, "VFS: File '%s' not found in archive '%s'", filePath, archiveKey.data());
        return nullptr;
    }

    mz_zip_archive_file_stat fileStat;
    if (!mz_zip_reader_file_stat(archiveReader, fileIndex, &fileStat))
    {
        TraceLog(LOG_ERROR, "VFS: Could not get file stat for '%s' in archive '%s'", filePath, archiveKey.data());
        return nullptr;
    }

    const size_t uncompressedSize = fileStat.m_uncomp_size;
    const size_t allocSize = static_cast<int>(uncompressedSize) + 1;

    unsigned char *fileData = static_cast<unsigned char *>(MemAlloc(allocSize));
    assert(fileData);

    if (!mz_zip_reader_extract_to_mem(archiveReader, fileIndex, fileData, uncompressedSize, 0))
    {
        TraceLog(LOG_ERROR, "VFS: Could not extract file '%s' from archive '%s'", filePath, archiveKey.data());
        MemFree(fileData);
        return nullptr;
    }

    dataSize = static_cast<int>(uncompressedSize);
    return fileData;
}

static unsigned char *FS_LoadFileData(const char *fileName, int &dataSize)
{
    assert(fileName);

    FILE *file = fopen(fileName, "rb");
    if (!file)
    {
        TraceLog(LOG_ERROR, "FS: Could not open file %s: %s", fileName, strerror(errno));
        return nullptr;
    }

    fseek(file, 0, SEEK_END);
    dataSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (dataSize <= 0)
    {
        fclose(file);
        return nullptr;
    }

    unsigned char *data = static_cast<unsigned char *>(MemAlloc(dataSize + 1));
    assert(data);

    size_t bytesRead = fread(data, 1, dataSize, file);
    if (bytesRead != static_cast<size_t>(dataSize))
    {
        TraceLog(LOG_ERROR, "FS: Could not read file %s: %s", fileName, strerror(errno));
        MemFree(data);
        fclose(file);
        return nullptr;
    }

    fclose(file);
    return data;
}

static bool FS_SaveFileData(const char *fileName, void *data, int dataSize);
static bool VFS_SaveFileData(const std::string &archiveKey, const char *filePath, void *data, int dataSize);

extern "C" bool SaveFileDataImpl(const char *filePath, void *data, int dataSize)
{
    if (!filePath || !data || dataSize < 0)
        return false;

    const std::string archiveKey = GetArchiveKeyFromPath(filePath);

    return archiveKey.empty()
               ? FS_SaveFileData(filePath, data, dataSize)
               : VFS_SaveFileData(archiveKey, filePath, data, dataSize);
}

extern "C" bool SaveFileTextImpl(const char *filePath, char *text)
{
    if (!filePath || !text)
        return false;

    const std::string archiveKey = GetArchiveKeyFromPath(filePath);

    return archiveKey.empty()
               ? FS_SaveFileData(filePath, text, strlen(text))
               : VFS_SaveFileData(archiveKey, filePath, text, strlen(text));
}

static bool VFS_SaveFileData(const std::string &archiveKey, const char *filePath, void *data, int dataSize)
{
    assert(!archiveKey.empty());
    assert(filePath);
    assert(data);
    assert(dataSize >= 0);

    auto it = g_DataArchives.find(archiveKey);
    if (it == g_DataArchives.end())
    {
        TraceLog(LOG_WARNING, "VFS: Archive key '%s' not found for saving file: %s", archiveKey.data(), filePath);
        return false;
    }

    ArchiveInfo &archiveInfo = it->second;
    auto archiveReader = archiveInfo.reader.get();

    if (!archiveReader || archiveReader->m_zip_mode == MZ_ZIP_MODE_INVALID)
    {
        TraceLog(LOG_WARNING, "VFS: Archive reader for key '%s' is not valid. Cannot save file: %s", archiveKey.data(), filePath);
        return false;
    }

    const std::string tempArchivePath = archiveInfo.fullPath + ".tmp";
    auto tmpArchive = std::make_unique<mz_zip_archive>();
    memset(tmpArchive.get(), 0, sizeof(mz_zip_archive));
    if (!mz_zip_writer_init_file(tmpArchive.get(), tempArchivePath.c_str(), 0))
    {
        TraceLog(LOG_ERROR, "VFS: Could not initialize temporary archive for saving file: %s", tempArchivePath.c_str());
        return false;
    }

    bool success = true;
    bool file_overwritten_or_added = false;

    mz_uint num_files = mz_zip_reader_get_num_files(archiveReader);
    for (mz_uint i = 0; i < num_files; ++i)
    {
        mz_zip_archive_file_stat fileStat;
        if (!mz_zip_reader_file_stat(archiveReader, i, &fileStat))
        {
            TraceLog(LOG_ERROR, "VFS: Could not get file stat for file %d in archive '%s'", i, archiveKey.data());
            success = false;
            break;
        }

        if (strcmp(fileStat.m_filename, filePath) == 0)
        {
            // Replace: Add the new data instead of copying the old version
            TraceLog(LOG_DEBUG, "VFS: Replacing '%s' in temporary archive.", filePath);
            if (!mz_zip_writer_add_mem(tmpArchive.get(), filePath, data, static_cast<size_t>(dataSize), MZ_DEFAULT_COMPRESSION))
            {
                TraceLog(LOG_ERROR, "VFS: Failed to add new data for '%s' to temp archive %s.", filePath, tempArchivePath.c_str());
                success = false;
                break;
            }
            file_overwritten_or_added = true;
        }
        else
        {
            // Copy: Keep this file from the original archive
            if (!mz_zip_writer_add_from_zip_reader(tmpArchive.get(), archiveReader, i))
            {
                TraceLog(LOG_ERROR, "VFS: Failed to copy file '%s' from archive '%s' to temp archive. Aborting save.", fileStat.m_filename, archiveKey.data());
                success = false;
                break;
            }
        }
    }

    if (success)
    {
        if (!file_overwritten_or_added)
        {
            // Create: Add the new file to the temporary archive
            if (!mz_zip_writer_add_mem(tmpArchive.get(), filePath, data, static_cast<size_t>(dataSize), MZ_DEFAULT_COMPRESSION))
            {
                TraceLog(LOG_ERROR, "VFS: Failed to add new file '%s' to temp archive %s.", filePath, tempArchivePath.c_str());
                success = false;
            }
        }

        if (!mz_zip_writer_finalize_archive(tmpArchive.get()))
        {
            TraceLog(LOG_ERROR, "VFS: Failed to finalize temporary archive: %s", tempArchivePath.c_str());
            success = false;
        }
    }

    if (!mz_zip_writer_end(tmpArchive.get()))
    {
        TraceLog(LOG_ERROR, "VFS: Failed to close temporary archive: %s", tempArchivePath.c_str());
        success = false;
    }
    tmpArchive.reset();

    if (!success)
    {
        TraceLog(LOG_WARNING, "VFS: Save operation failed for '%s'. Cleaning up temporary file: %s", filePath, tempArchivePath.c_str());
        remove(tempArchivePath.c_str());
        return false;
    }

    // Close reader for the original file
    mz_zip_reader_end(archiveInfo.reader.get());
    archiveInfo.reader.reset();

    // Backup the original archive
    auto backup = archiveInfo.fullPath + ".bak";
    if (rename(archiveInfo.fullPath.c_str(), backup.c_str()) != 0)
    {
        TraceLog(LOG_ERROR, "VFS: Failed to rename original archive %s to backup %s", archiveInfo.fullPath.c_str(), backup.c_str());
        remove(tempArchivePath.c_str());

        // Attempt to reinitialize the reader
        auto archiveReader = std::make_unique<mz_zip_archive>();
        memset(archiveReader.get(), 0, sizeof(mz_zip_archive));
        if (!mz_zip_reader_init_file(archiveReader.get(), archiveInfo.fullPath.c_str(), 0))
        {
            TraceLog(LOG_ERROR, "VFS: Failed to reinitialize reader for original archive %s", archiveInfo.fullPath.c_str());
            return false;
        }
        archiveInfo.reader = std::move(archiveReader);
        return false;
    }

    // Replace the original archive with the temporary one
    if (rename(tempArchivePath.c_str(), archiveInfo.fullPath.c_str()) != 0)
    {
        TraceLog(LOG_ERROR, "VFS: Failed to rename temporary archive %s to original %s", tempArchivePath.c_str(), archiveInfo.fullPath.c_str());
        remove(tempArchivePath.c_str());

        // Restore backup
        if (rename(backup.c_str(), archiveInfo.fullPath.c_str()) != 0)
        {
            // In this case, we can't do anything
            TraceLog(LOG_ERROR, "VFS: Failed to restore backup archive %s to original %s", backup.c_str(), archiveInfo.fullPath.c_str());
            return false;
        }

        // Attempt to reinitialize the reader
        auto archiveReader = std::make_unique<mz_zip_archive>();
        memset(archiveReader.get(), 0, sizeof(mz_zip_archive));
        if (!mz_zip_reader_init_file(archiveReader.get(), archiveInfo.fullPath.c_str(), 0))
        {
            TraceLog(LOG_ERROR, "VFS: Failed to reinitialize reader for original archive %s", archiveInfo.fullPath.c_str());
            return false;
        }
        archiveInfo.reader = std::move(archiveReader);
        return false;
    }
    else
    {
        remove(backup.c_str());
    }

    // Reinitialize the reader for the new archive
    auto newArchiveReader = std::make_unique<mz_zip_archive>();
    memset(newArchiveReader.get(), 0, sizeof(mz_zip_archive));
    if (!mz_zip_reader_init_file(newArchiveReader.get(), archiveInfo.fullPath.c_str(), 0))
    {
        TraceLog(LOG_ERROR, "VFS: Failed to reinitialize reader for updated archive %s", archiveInfo.fullPath.c_str());
        return false;
    }

    archiveInfo.reader = std::move(newArchiveReader);
    return true;
}

static bool FS_SaveFileData(const char *fileName, void *data, int dataSize)
{
    assert(fileName);
    assert(data);
    assert(dataSize >= 0);

    FILE *file = fopen(fileName, "wb");
    if (!file)
    {
        TraceLog(LOG_ERROR, "FS: Could not open file %s for writing: %s", fileName, strerror(errno));
        return false;
    }

    size_t bytesWritten = fwrite(data, 1, dataSize, file);
    fclose(file);

    if (bytesWritten != static_cast<size_t>(dataSize))
    {
        TraceLog(LOG_ERROR, "FS: Could not write to file %s: %s", fileName, strerror(errno));
        return false;
    }

    return true;
}
