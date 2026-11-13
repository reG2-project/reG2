#!/bin/sh
# build.sh <linux32|linux64|mingw32|mingw64|macos> <deps prefix> <build dir>: the port, with the libraries deps.sh
# installed. The Linux builds link against the system root of deps.sh, with the C++ runtime static, and find
# libSDL3.so.0 next to themselves (linux64 also in ../lib, the AppImage's layout). macos makes a universal reg2.app
# (arm64 and x86_64). mingw64 fails on a 64-bit value truncated into a long, 32 bits on Windows and 64 on Linux.
# $GTA2_VERSION, if set, is the version (a leading v is dropped: the tag v0.2.0 gives 0.2.0).
set -eu
T=$1; P=$2; B=$3
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$(cd "$HERE/.." && pwd)
case $T in
    linux32 | linux64)
        export GTA2_SYSROOT="$P/sysroot"
        RPATH='$ORIGIN'
        [ "$T" = linux64 ] && RPATH='$ORIGIN:$ORIGIN/../lib'
        set -- -DCMAKE_TOOLCHAIN_FILE="$HERE/$T-sysroot.cmake" -DCMAKE_PREFIX_PATH="$P" -DSDL3_DIR="$P/lib/cmake/SDL3" \
            -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON "-DCMAKE_INSTALL_RPATH=$RPATH" \
            "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc -Wl,--disable-new-dtags -Wl,-z,now"
        ;;
    mingw32 | mingw64)
        set -- -DCMAKE_TOOLCHAIN_FILE="$PORT/toolchain-$T-clang.cmake" -DSDL3_DIR="$P/sdl3/lib/cmake/SDL3"
        if [ "$T" = mingw64 ]; then
            set -- "$@" -DGTA2_WARN_SHORTEN=ON
        fi
        ;;
    macos) set -- -DCMAKE_PREFIX_PATH="$P" -DSDL3_DIR="$P/lib/cmake/SDL3" "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" ;;
    *) echo "build.sh: unknown target $T" >&2; exit 1 ;;
esac
cmake -S "$PORT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@" -DGTA2_SETUP_DEPS_DIR="$P" -DGTA2_OPENAL_DIR="$P" \
    ${GTA2_VERSION:+"-DGTA2_VERSION=${GTA2_VERSION#v}"}
if [ "$T" != mingw64 ]; then
    ninja -C "$B" -j"$(getconf _NPROCESSORS_ONLN)"
    exit
fi
ninja -C "$B" -j"$(getconf _NPROCESSORS_ONLN)" > "$B/build.log" 2>&1 || { cat "$B/build.log"; exit 1; }
cat "$B/build.log"
# The destination type, or the type a typedef stands for: 'long', 'LONG' (aka 'long'), 'unsigned long'. Lengths
# (size_t, ptrdiff_t) are left out: they go to the Win32 API's DWORD sizes, they are not handles or pointers.
if grep -E "to '(const )?(unsigned )?long'|to '[^']*' \(aka '(const )?(unsigned )?long'\)" "$B/build.log" | grep -F -- '-Wshorten-64-to-32' |
    grep -Ev "precision: '(size_t|__ptrdiff_t|ptrdiff_t)'" | sort -u | grep .; then
    echo "build.sh: 64-bit values truncated into a long, 32 bits on Windows" >&2
    exit 1
fi
