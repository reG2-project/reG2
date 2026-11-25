#!/bin/sh
# deps.sh <downloads> <prefix> <build dir>: SDL3, zlib, xz, unshield and libarchive for WebAssembly, from the
# sources of ../packaging/versions.sh, installed into <prefix>. Needs emsdk's environment (emcmake on the PATH).
set -eu
DL=$1; P=$2; B=$3
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$P" "$B"
J=$(getconf _NPROCESSORS_ONLN)
sh "$HERE/../packaging/fetch.sh" "$DL" SDL3-3.4.18.tar.gz zlib-1.3.2.tar.gz xz-5.8.4.tar.gz unshield-1.6.2.tar.gz \
    libarchive-3.8.11.tar.xz
unpack() { tar -xf "$DL/$1" -C "$B"; }
C="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$P -DCMAKE_FIND_ROOT_PATH=$P"

unpack SDL3-3.4.18.tar.gz
emcmake cmake -S "$B/SDL3-3.4.18" -B "$B/b_sdl3" $C -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF \
      -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF -DSDL_PTHREADS=OFF -DSDL_THREADS=OFF
ninja -C "$B/b_sdl3" -j"$J" install
unpack zlib-1.3.2.tar.gz
emcmake cmake -S "$B/zlib-1.3.2" -B "$B/b_zlib" $C -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_TESTING=OFF
ninja -C "$B/b_zlib" -j"$J" install
unpack xz-5.8.4.tar.gz
emcmake cmake -S "$B/xz-5.8.4" -B "$B/b_xz" $C -DBUILD_SHARED_LIBS=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF \
      -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_TOOL_SYMLINKS=OFF \
      -DXZ_TOOL_SYMLINKS_LZMA=OFF -DXZ_DOC=OFF -DXZ_NLS=OFF -DXZ_THREADS=no -DBUILD_TESTING=OFF
ninja -C "$B/b_xz" -j"$J" install
Z=$P/lib/libzs.a
[ -f "$Z" ] || Z=$P/lib/libz.a
C="$C -DZLIB_LIBRARY=$Z -DZLIB_INCLUDE_DIR=$P/include -DLIBLZMA_LIBRARY=$P/lib/liblzma.a
   -DLIBLZMA_INCLUDE_DIR=$P/include -DCMAKE_C_FLAGS=-DLZMA_API_STATIC"

unpack unshield-1.6.2.tar.gz
emcmake cmake -S "$B/unshield-1.6.2" -B "$B/b_unshield" $C -DBUILD_STATIC=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
ninja -C "$B/b_unshield" -j"$J" install
# unshield's static library leaves out its own MD5 objects.
emar rs "$P/lib/libunshield.a" "$B"/b_unshield/lib/md5/CMakeFiles/md5.dir/md5c.c.o
unpack libarchive-3.8.11.tar.xz
emcmake cmake -S "$B/libarchive-3.8.11" -B "$B/b_libarchive" $C -DBUILD_SHARED_LIBS=OFF -DENABLE_OPENSSL=OFF \
      -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_MD=OFF -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_ZSTD=OFF \
      -DENABLE_BZip2=OFF -DENABLE_LIBXML2=OFF -DENABLE_BSDXML=OFF -DENABLE_EXPAT=OFF -DENABLE_PCREPOSIX=OFF \
      -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
      -DENABLE_UNZIP=OFF -DENABLE_XATTR=OFF -DENABLE_ACL=OFF -DENABLE_ICONV=OFF -DENABLE_TEST=OFF -DPOSIX_REGEX_LIB=NONE
ninja -C "$B/b_libarchive" -j"$J" install
# The licences the site serves.
mkdir -p "$P/licenses"
cp "$B/SDL3-3.4.18/LICENSE.txt" "$P/licenses/SDL3.txt"
cp "$B/unshield-1.6.2/LICENSE" "$P/licenses/unshield.txt"
cp "$B/libarchive-3.8.11/COPYING" "$P/licenses/libarchive.txt"
cp "$B/zlib-1.3.2/LICENSE" "$P/licenses/zlib.txt"
cat "$B/xz-5.8.4/COPYING" "$B/xz-5.8.4/COPYING.0BSD" > "$P/licenses/xz.txt"
cp "$HERE/../3rdParty/libbink/LICENSE" "$P/licenses/libbink.txt"
touch "$P/.complete"
