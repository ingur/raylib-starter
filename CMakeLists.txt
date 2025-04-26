cmake_minimum_required(VERSION 3.16)

# Set your project name/version here
project(game)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# savely cache the project name for build script
set(CACHED_PROJECT_NAME ${PROJECT_NAME} CACHE STRING "Cached project name")

# --- Project Structure ---

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(INC_DIR "${CMAKE_SOURCE_DIR}/include")
set(LIB_DIR "${CMAKE_SOURCE_DIR}/lib")

# --- Source Files ---

set(SRCS
    "${INC_DIR}/miniz/miniz.c"
    "${SRC_DIR}/main.cpp"
    # Add other source files here
)

# --- Target Definition ---

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${PROJECT_NAME}")
add_executable(${PROJECT_NAME} ${SRCS})
target_include_directories(${PROJECT_NAME} PRIVATE ${INC_DIR})

# --- Build Configuration ---

# Enable Link-Time Optimization for Release builds
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
if(build_type_upper STREQUAL "RELEASE")
    message(STATUS "Enabling Link-Time Optimization for Release build")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# Use the dynamic libraries in the same directory as the executable
set(CMAKE_INSTALL_RPATH "$ORIGIN")

# Add large file support for miniz
target_compile_definitions(${PROJECT_NAME} PRIVATE _LARGEFILE64_SOURCE)

# --- Platform Specific Settings ---

if(NOT DEFINED PLATFORM)
    message(FATAL_ERROR "PLATFORM variable not set.")
endif()

if(${PLATFORM} STREQUAL "linux_x86_64")
    set(PLATFORM_LIB_DIR "${LIB_DIR}/linux_x86_64")
    target_link_libraries(
        ${PROJECT_NAME} PRIVATE
        "${PLATFORM_LIB_DIR}/libraylib.so"
        "${PLATFORM_LIB_DIR}/libluajit.so"
        GL m pthread dl rt X11 # System libraries
    )
    # Enable dynamic linking for libraries
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
elseif(${PLATFORM} STREQUAL "windows_x86_64")
    set(PLATFORM_LIB_DIR "${LIB_DIR}/windows_x86_64")
    target_link_libraries(
        ${PROJECT_NAME} PRIVATE
        "${PLATFORM_LIB_DIR}/raylib.dll"
        "${PLATFORM_LIB_DIR}/lua51.dll"
        opengl32 gdi32 winmm 
    )
    # Linker flags for MinGW
    target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
else()
    message(FATAL_ERROR "Unsupported platform: ${PLATFORM}")
endif()
