#include "vfs.h"

#include "raylib.h"
#include "pocketpy.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mz_zip_archive zip;
static unsigned char *blob;
static bool mounted;

typedef void *(*Alloc)(size_t size);
typedef void (*Dealloc)(void *ptr);

static void *AllocRl(size_t size) { return MemAlloc((unsigned int)(size + 1)); }
static void *AllocPk(size_t size) { return PK_MALLOC(size + 1); }
static void *AllocStd(size_t size) { return malloc(size + 1); }

static void FreeRl(void *ptr) { MemFree(ptr); }
static void FreePk(void *ptr) { PK_FREE(ptr); }

static void *ReadDisk(const char *path, int *size, Alloc alloc, Dealloc dealloc) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }

    unsigned char *data = alloc((size_t)len);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    size_t read = fread(data, 1, (size_t)len, f);
    fclose(f);
    if (read != (size_t)len) {
        dealloc(data);
        return NULL;
    }
    data[len] = '\0';
    *size = (int)len;
    return data;
}

static void *ReadPak(const char *path, int *size, Alloc alloc, Dealloc dealloc) {
    if (!mounted) return NULL;
    int index = mz_zip_reader_locate_file(&zip, path, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0) return NULL;

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, index, &stat)) return NULL;

    size_t len = (size_t)stat.m_uncomp_size;
    unsigned char *data = alloc(len);
    if (data == NULL) return NULL;
    if (!mz_zip_reader_extract_to_mem(&zip, index, data, len, 0)) {
        dealloc(data);
        return NULL;
    }
    data[len] = '\0';
    *size = (int)len;
    return data;
}

static void *ReadAny(const char *path, int *size, Alloc alloc, Dealloc dealloc) {
    void *data = ReadDisk(path, size, alloc, dealloc);
    return data ? data : ReadPak(path, size, alloc, dealloc);
}

static unsigned char *LoadData(const char *fileName, int *dataSize) {
    *dataSize = 0;
    unsigned char *data = ReadAny(fileName, dataSize, AllocRl, FreeRl);
    if (data == NULL) TraceLog(LOG_WARNING, "VFS: [%s] not found", fileName);
    return data;
}

static char *LoadText(const char *fileName) {
    int size = 0;
    return ReadAny(fileName, &size, AllocRl, FreeRl);
}

char *ImportFile(const char *path, int *dataSize) {
    int size = 0;
    char prefixed[512];
    snprintf(prefixed, sizeof(prefixed), "game/%s", path);

    char *data = ReadAny(prefixed, &size, AllocPk, FreePk);
    if (data == NULL) data = ReadAny(path, &size, AllocPk, FreePk);
    if (dataSize) *dataSize = size;
    return data;
}

bool MountAssets(void) {
    SetLoadFileDataCallback(LoadData);
    SetLoadFileTextCallback(LoadText);

#if defined(__EMSCRIPTEN__)
    const char *path = "/" ASSETS_PAK;
#else
    const char *path = TextFormat("%s%s", GetApplicationDirectory(), ASSETS_PAK);
#endif

    int size = 0;
    blob = ReadDisk(path, &size, AllocStd, free);
    if (blob == NULL) {
        TraceLog(LOG_INFO, "VFS: %s not found, using loose files", path);
        return false;
    }

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, blob, (size_t)size, 0)) {
        TraceLog(LOG_WARNING, "VFS: %s is not a valid pak", path);
        free(blob);
        blob = NULL;
        return false;
    }

    mounted = true;
    TraceLog(LOG_INFO, "VFS: mounted %s (%d files)", path, (int)mz_zip_reader_get_num_files(&zip));
    return true;
}

void UnmountAssets(void) {
    SetLoadFileDataCallback(NULL);
    SetLoadFileTextCallback(NULL);
    if (!mounted) return;
    mz_zip_reader_end(&zip);
    free(blob);
    blob = NULL;
    mounted = false;
}
