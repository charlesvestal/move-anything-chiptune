#!/usr/bin/env bash
# Build Chiptune module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Chiptune Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Chiptune Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/chiptune

# Compile NES APU library (C++)
echo "Compiling NES APU library..."
NES_SRCS="
    src/libs/nes_snd_emu/nes_apu/Nes_Apu.cpp
    src/libs/nes_snd_emu/nes_apu/Nes_Oscs.cpp
    src/libs/nes_snd_emu/nes_apu/Blip_Buffer.cpp
"

for src in $NES_SRCS; do
    obj="build/$(basename "$src" .cpp).o"
    echo "  $src -> $obj"
    ${CROSS_PREFIX}g++ -g -O3 -fPIC -std=c++14 \
        -I src/libs/nes_snd_emu \
        -c "$src" \
        -o "$obj"
done

# Compile GB APU library (blargg's Gb_Snd_Emu, C++)
echo "Compiling GB APU library (blargg)..."
GB_SRCS="
    src/libs/gb_snd_emu/Gb_Apu.cpp
    src/libs/gb_snd_emu/Gb_Oscs.cpp
    src/libs/gb_snd_emu/Blip_Buffer.cpp
    src/libs/gb_snd_emu/Multi_Buffer.cpp
    src/libs/gb_snd_emu/gb_apu_wrapper.cpp
"

for src in $GB_SRCS; do
    obj="build/gb_$(basename "$src" .cpp).o"
    echo "  $src -> $obj"
    ${CROSS_PREFIX}g++ -g -O3 -fPIC -std=c++14 -fvisibility=hidden \
        -I src/libs/gb_snd_emu \
        -c "$src" \
        -o "$obj"
done

# Partial-link GB objects into one .o so hidden Blip_Buffer symbols
# don't collide with NES Blip_Buffer at final link
echo "Partial-linking GB APU objects..."
${CROSS_PREFIX}ld -r \
    build/gb_Gb_Apu.o \
    build/gb_Gb_Oscs.o \
    build/gb_Blip_Buffer.o \
    build/gb_Multi_Buffer.o \
    build/gb_gb_apu_wrapper.o \
    -o build/gb_apu_combined.o
${CROSS_PREFIX}objcopy --localize-hidden build/gb_apu_combined.o

# Compile plugin wrapper (C++)
echo "Compiling plugin wrapper..."
${CROSS_PREFIX}g++ -g -O3 -fPIC -std=c++14 \
    -I src/dsp \
    -I src/libs/nes_snd_emu \
    -I src/libs/gb_snd_emu \
    -c src/dsp/chiptune_plugin.cpp \
    -o build/chiptune_plugin.o

# Link shared library
echo "Linking dsp.so..."
${CROSS_PREFIX}g++ -shared \
    build/chiptune_plugin.o \
    build/Nes_Apu.o \
    build/Nes_Oscs.o \
    build/Blip_Buffer.o \
    build/gb_apu_combined.o \
    -o build/dsp.so \
    -lm

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/chiptune/module.json
[ -f src/help.json ] && cat src/help.json > dist/chiptune/help.json
cat build/dsp.so > dist/chiptune/dsp.so
chmod +x dist/chiptune/dsp.so

# Include chain patches in dist
if [ -d "src/chain_patches" ]; then
    mkdir -p dist/chiptune/chain_patches
    for f in src/chain_patches/*.json; do
        [ -f "$f" ] && cat "$f" > "dist/chiptune/chain_patches/$(basename "$f")"
    done
fi

# Create tarball for release
cd dist
tar -czvf chiptune-module.tar.gz chiptune/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/chiptune/"
echo "Tarball: dist/chiptune-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
