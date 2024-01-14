cmake_minimum_required(VERSION 3.14)
project(game)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Directories
set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(INC_DIR "${CMAKE_SOURCE_DIR}/include")
set(LIB_DIR "${CMAKE_SOURCE_DIR}/lib")

# Source files
file(GLOB_RECURSE SRCS "${SRC_DIR}/*.cpp")

# setup target executable
add_executable(${PROJECT_NAME} ${SRCS})
target_include_directories(${PROJECT_NAME} PRIVATE ${INC_DIR})

# platform specific settings
if(EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "index")
    target_compile_options(${PROJECT_NAME} PRIVATE "-DPLATFORM_WEB")
    target_link_libraries(
        ${PROJECT_NAME} 
        "${LIB_DIR}/web/libraylib.a" 
        "-s USE_GLFW=3" 
        -sGL_ENABLE_GET_PROC_ADDRESS 
        "--preload-file ${CMAKE_SOURCE_DIR}/assets@assets"
        "--shell-file ${CMAKE_SOURCE_DIR}/shell.html"
    )
elseif(${PLATFORM} STREQUAL "linux")
    target_link_libraries(
        ${PROJECT_NAME}
        "${LIB_DIR}/linux/libraylib.a"
        -lGL -lm -lpthread -ldl -lrt -lX11
    )
elseif(${PLATFORM} STREQUAL "windows")
    target_link_libraries(
        ${PROJECT_NAME}
        "${LIB_DIR}/windows/libraylib.a" 
        -lopengl32 -lgdi32 -lwinmm
    )
endif()

