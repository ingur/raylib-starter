set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(prefix x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${prefix}-gcc)
set(CMAKE_CXX_COMPILER ${prefix}-g++)
set(CMAKE_RC_COMPILER ${prefix}-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Static runtimes, so the .exe needs no extra DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
