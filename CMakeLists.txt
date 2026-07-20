cmake_minimum_required(VERSION 3.22)

# Rename the binary here.
project(game C)

# Update dependencies here. Any git tag, branch or commit works.
# After a raylib bump, rerun `bindgen` to refresh the python bindings.
set(RAYLIB_VERSION "6.0" CACHE STRING "raylib git ref")
set(POCKETPY_VERSION "v2.1.8" CACHE STRING "pocketpy git ref")

# Packed assets file name. The pak is a plain zip.
set(ASSETS_PAK "assets.pak" CACHE STRING "packed assets file name")

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
set(PK_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
set(PK_ENABLE_DLL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG ${RAYLIB_VERSION}
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_Declare(
  pocketpy
  GIT_REPOSITORY https://github.com/pocketpy/pocketpy.git
  GIT_TAG ${POCKETPY_VERSION}
  GIT_SHALLOW TRUE
  # mingw headers are lowercase, upstream include only works on MSVC
  PATCH_COMMAND sed -i "s/<WinSock2.h>/<winsock2.h>/" src/common/socket.c
  SYSTEM
)
FetchContent_Declare(
  miniz
  URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
  URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(raylib pocketpy miniz)

file(GLOB SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.c")

add_executable(${PROJECT_NAME} ${SOURCES} ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${miniz_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE raylib pocketpy)
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)
target_compile_definitions(${PROJECT_NAME} PRIVATE ASSETS_PAK="${ASSETS_PAK}")

set_target_properties(${PROJECT_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
  WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)  # hide the Windows console in release

# Pack assets/ and game/ into a zip beside the binary.
file(GLOB_RECURSE PAK_FILES CONFIGURE_DEPENDS assets/* game/*)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/$<CONFIG>/${ASSETS_PAK}
  COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/$<CONFIG>/${ASSETS_PAK} --format=zip assets game
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS ${PAK_FILES}
  COMMENT "Packing assets" VERBATIM)
add_custom_target(assets_pak ALL DEPENDS ${CMAKE_BINARY_DIR}/$<CONFIG>/${ASSETS_PAK})
add_dependencies(${PROJECT_NAME} assets_pak)

add_custom_target(run
  COMMAND $<TARGET_FILE:${PROJECT_NAME}>
  DEPENDS ${PROJECT_NAME}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL)

if(EMSCRIPTEN)
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
  target_link_options(${PROJECT_NAME} PRIVATE -sALLOW_MEMORY_GROWTH=1 -lidbfs.js
    "SHELL:--preload-file ${CMAKE_BINARY_DIR}/$<CONFIG>/${ASSETS_PAK}@/${ASSETS_PAK}"
    "SHELL:--pre-js ${CMAKE_SOURCE_DIR}/src/web_save.js")
  # The pak and pre-js are baked in at link time, relink when they change.
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_BINARY_DIR}/$<CONFIG>/${ASSETS_PAK} ${CMAKE_SOURCE_DIR}/src/web_save.js)
endif()
