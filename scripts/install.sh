#!/bin/bash
# Install Chiptune module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/chiptune" ]; then
    echo "Error: dist/chiptune not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Chiptune Module ==="

# Deploy to Move - sound_generators subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/sound_generators/chiptune"
scp -r dist/chiptune/* ableton@move.local:/data/UserData/move-anything/modules/sound_generators/chiptune/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    if ls src/chain_patches/*.json 1>/dev/null 2>&1; then
        scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/patches/
    fi
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/sound_generators/chiptune"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/sound_generators/chiptune/"
echo ""
echo "Restart Move Anything to load the new module."
