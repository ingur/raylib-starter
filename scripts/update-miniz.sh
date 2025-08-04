#!/bin/bash
set -euo pipefail

echo "=== Updating miniz ==="

# Create temp directory
TEMP_DIR=$(mktemp -d)
cd "$TEMP_DIR"

# Clone miniz
echo "Cloning miniz repository..."
git clone https://github.com/richgel999/miniz.git
cd miniz

# Get latest tag/version
LATEST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "No tags")
echo "Latest version: $LATEST_TAG"

# Run amalgamation script
echo "Running amalgamation..."
chmod +x amalgamate.sh
./amalgamate.sh

# Copy files to project
echo "Copying files..."
cp amalgamation/miniz.c "/workspace/include/miniz/"
cp amalgamation/miniz.h "/workspace/include/miniz/"

# Cleanup
cd "$OLDPWD"
rm -rf "$TEMP_DIR"

echo "✓ miniz updated successfully"
