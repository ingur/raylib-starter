#!/bin/bash
set -euo pipefail

echo "=== Updating all libraries ==="

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Update miniz
echo ""
echo "1/2: Updating miniz..."
"$SCRIPT_DIR/update-miniz.sh"

# Update LuaJIT
echo ""
echo "2/3: Updating LuaJIT..."
"$SCRIPT_DIR/update-luajit.sh"

# Update raylib
echo ""
echo "3/3: Updating raylib..."
"$SCRIPT_DIR/update-raylib.sh"

echo ""
echo "✓ All libraries updated successfully"