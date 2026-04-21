#!/bin/sh
# fetch-gcc-4.8.sh — download & configure gcc 4.8.5 as a sea-front test target.
#
# sea-front's primary integration target is compiling gcc 4.8 source through
# the preprocess → sea-front → cc pipeline. This script stages the source
# tree and a build dir with libstdc++ headers pre-configured, in a stable
# location that survives reboots (unlike /tmp).
#
# Usage:  scripts/fetch-gcc-4.8.sh
#
# Env overrides:
#   SEA_DEPS_DIR   — where to stage source/build trees
#                    (default: ~/src/sea-front-deps)
#   GCC_VERSION    — gcc tag to fetch (default: 4.8.5)
#
# Produces:
#   $SEA_DEPS_DIR/gcc-4.8.5/            — extracted source
#   $SEA_DEPS_DIR/gcc-4.8.5/build-sf/   — configured build dir
#                                         with libstdc++ headers generated
#
# After running, set:
#   export SEA_LIBSTDCXX_SRC=$SEA_DEPS_DIR/gcc-4.8.5/libstdc++-v3/include
#   export SEA_LIBSTDCXX_BUILD=$SEA_DEPS_DIR/gcc-4.8.5/build-sf/x86_64-unknown-linux-gnu/libstdc++-v3/include

set -e

SEA_DEPS_DIR="${SEA_DEPS_DIR:-$HOME/src/sea-front-deps}"
GCC_VERSION="${GCC_VERSION:-4.8.5}"
GCC_DIR="$SEA_DEPS_DIR/gcc-$GCC_VERSION"
BUILD_DIR="$GCC_DIR/build-sf"
TARBALL="gcc-$GCC_VERSION.tar.gz"
MIRROR="https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION"

mkdir -p "$SEA_DEPS_DIR"
cd "$SEA_DEPS_DIR"

# Step 1: Fetch tarball if not already present.
if [ ! -f "$TARBALL" ]; then
    echo "Fetching $TARBALL from $MIRROR..."
    curl -fLO "$MIRROR/$TARBALL"
fi

# Step 2: Extract if source tree not already present.
if [ ! -d "$GCC_DIR" ]; then
    echo "Extracting $TARBALL..."
    tar xf "$TARBALL"
fi

# Step 3: Download prerequisites (gmp, mpfr, mpc, etc.) via gcc's helper.
#         These land in-tree as sibling dirs so in-tree build finds them.
cd "$GCC_DIR"
if [ ! -d gmp ] || [ ! -d mpfr ] || [ ! -d mpc ]; then
    echo "Downloading gcc prerequisites..."
    ./contrib/download_prerequisites
fi

# Step 4: Top-level configure — generates auto-host.h, gcc/config.h,
#         gcc/Makefile, etc. We don't run `make` (gcc 4.8 source doesn't
#         compile cleanly under modern g++, warnings-as-errors on
#         fallthrough), but the configure outputs are enough for
#         sea-front to preprocess gcc/*.c source files (which #include
#         "config.h", "auto-host.h").
if [ ! -f "$BUILD_DIR/gcc/config.h" ]; then
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    echo "Configuring gcc-$GCC_VERSION (top-level)..."
    ../configure \
        --prefix="$BUILD_DIR/install" \
        --enable-languages=c,c++ \
        --disable-multilib \
        --disable-bootstrap \
        --disable-nls
    # 'configure' alone doesn't descend into per-subdir configures
    # (that happens during `make`). We need gcc/config.h for gcc/*.c
    # compilations. Invoke configure-gcc then build the cs-* header
    # stubs (tiny wrappers over auto-host.h).
    echo "Configuring gcc/ subdir..."
    make configure-gcc
    cd gcc
    make cs-config.h cs-bconfig.h cs-tconfig.h
fi

# Step 5: Configure libstdc++-v3 directly to generate bits/c++config.h
#         and per-target headers. libstdc++-v3's own configure runs fine
#         with the host toolchain.
LIBSTDCXX_BUILD_DIR="$BUILD_DIR/x86_64-unknown-linux-gnu/libstdc++-v3"
if [ ! -f "$LIBSTDCXX_BUILD_DIR/include/x86_64-unknown-linux-gnu/bits/c++config.h" ]; then
    mkdir -p "$LIBSTDCXX_BUILD_DIR"
    cd "$LIBSTDCXX_BUILD_DIR"
    echo "Configuring libstdc++-v3..."
    "$GCC_DIR/libstdc++-v3/configure" \
        --host=x86_64-unknown-linux-gnu \
        --enable-multilib=no
    echo "Generating per-target headers..."
    make -j include
fi
LIBSTDCXX_BUILD="$LIBSTDCXX_BUILD_DIR/include"

echo ""
echo "=== Done ==="
echo "Source:  $GCC_DIR"
echo "Build:   $BUILD_DIR"
echo ""
echo "Set these env vars for sea-front-cc:"
echo "  export SEA_LIBSTDCXX_SRC=$GCC_DIR/libstdc++-v3/include"
echo "  export SEA_LIBSTDCXX_BUILD=$LIBSTDCXX_BUILD"
