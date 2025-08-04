#!/bin/bash
set -euo pipefail

echo "=== Updating LuaJIT ==="

# Create temp directory
TEMP_DIR=$(mktemp -d)
cd "$TEMP_DIR"

# Clone LuaJIT v2.1 branch
echo "Cloning LuaJIT v2.1 branch..."
git clone -b v2.1 --single-branch https://github.com/LuaJIT/LuaJIT.git
cd LuaJIT

# Show latest commit
echo "Latest commit:"
git log -1 --oneline

# Build for Linux
echo "Building LuaJIT for Linux..."
make clean
make -j$(nproc)

# Copy headers
echo "Copying headers..."
cp src/{lauxlib.h,lua.h,lua.hpp,luaconf.h,luajit.h,lualib.h} "/workspace/include/luajit/"

# Patch and copy Linux library
echo "Patching and copying Linux library..."
patchelf --set-soname libluajit.so src/libluajit.so
cp src/libluajit.so "/workspace/lib/linux_x86_64/"

# Build for Windows
echo "Building LuaJIT for Windows..."
make clean
make HOST_CC=gcc CROSS=x86_64-w64-mingw32- TARGET_SYS=Windows -j$(nproc)

# Copy Windows library
echo "Copying Windows library..."
cp src/lua51.dll "/workspace/lib/windows_x86_64/"

# Cleanup
cd "$OLDPWD"
rm -rf "$TEMP_DIR"

echo "✓ LuaJIT updated successfully"