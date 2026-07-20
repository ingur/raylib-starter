#pragma once

#include <stdbool.h>

bool MountAssets(void);
void UnmountAssets(void);

// pocketpy importfile callback. Resolves scripts from game/ on disk first,
// then from the pak. Buffer is PK_MALLOC'd and NUL terminated.
char *ImportFile(const char *path, int *dataSize);
