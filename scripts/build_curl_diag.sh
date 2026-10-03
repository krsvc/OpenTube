#!/bin/sh
# Rebuilds the SAME bundled libcurl 7.82.0 (library/libcurl/3ds.patch, same configure options) against the
# INSTALLED devkitPro 3ds-mbedtls, for the diagnostic build only. Output: deps/curl-7.82.0-mbedtls<ver>/lib/libcurl.a
# Never touches library/libcurl/*, /opt, or installs packages. At most 3 jobs.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${DEVKITPRO:?DEVKITPRO must be set}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitARM/bin:$PATH"
for t in arm-none-eabi-gcc gpg xz curl patch; do command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t"; exit 2; }; done
MBEDTLS_VER="$(sed -n 's/#define MBEDTLS_VERSION_STRING  *"\(.*\)"/\1/p' "$DEVKITPRO/portlibs/3ds/include/mbedtls/version.h")"
[ -n "$MBEDTLS_VER" ] || { echo "3ds-mbedtls not found in $DEVKITPRO/portlibs/3ds"; exit 2; }
CURL_VER=7.82.0
DEPS="$ROOT/deps"; SRC="$DEPS/src"; OUT="$DEPS/curl-$CURL_VER-mbedtls$MBEDTLS_VER"; WORK="$SRC/curl-$CURL_VER-3ds"
JOBS="${JOBS:-3}"
mkdir -p "$SRC" "$OUT/lib" "$OUT/include"
cd "$SRC"
[ -f "curl-$CURL_VER.tar.xz" ]     || curl -sSfLo "curl-$CURL_VER.tar.xz"     "https://curl.se/download/curl-$CURL_VER.tar.xz"
[ -f "curl-$CURL_VER.tar.xz.asc" ] || curl -sSfLo "curl-$CURL_VER.tar.xz.asc" "https://curl.se/download/curl-$CURL_VER.tar.xz.asc"
[ -f daniel-mykey.asc ]            || curl -sSfLo daniel-mykey.asc "https://daniel.haxx.se/mykey.asc"
# publisher signature check with a throwaway keyring (the user's keyring is never touched)
GNUPGHOME_TMP="$DEPS/gnupg"; mkdir -p "$GNUPGHOME_TMP"; chmod 700 "$GNUPGHOME_TMP"
gpg --homedir "$GNUPGHOME_TMP" --batch --quiet --import daniel-mykey.asc 2>/dev/null || true
EXPECTED_FPR=27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2   # Daniel Stenberg, published at https://daniel.haxx.se/mykey.asc
STATUS="$(gpg --homedir "$GNUPGHOME_TMP" --batch --status-fd 1 --verify "curl-$CURL_VER.tar.xz.asc" "curl-$CURL_VER.tar.xz" 2>/dev/null || true)"
echo "$STATUS" | grep -q "^\[GNUPG:\] VALIDSIG $EXPECTED_FPR" || { echo "curl tarball signature NOT valid for $EXPECTED_FPR"; echo "$STATUS"; exit 3; }
echo "signature OK: curl-$CURL_VER.tar.xz signed by $EXPECTED_FPR"
shasum -a 256 "curl-$CURL_VER.tar.xz" | tee "$OUT/SOURCE_SHA256"
if [ ! -d "$WORK" ]; then
	mkdir -p "$WORK.tmp" && tar -xJf "curl-$CURL_VER.tar.xz" -C "$WORK.tmp" && mv "$WORK.tmp/curl-$CURL_VER" "$WORK" && rmdir "$WORK.tmp"
	(cd "$WORK" && patch -Np1 -i "$ROOT/library/libcurl/3ds.patch")
fi
# curl's configure probes -lbrotlidec / -lbrotlicommon; the bundled archive is a single libbrotli.a
SHIM="$DEPS/brotli-shim/lib"; mkdir -p "$SHIM"
ln -sf "$ROOT/library/libbrotli/lib/libbrotli.a" "$SHIM/libbrotlidec.a"
ln -sf "$ROOT/library/libbrotli/lib/libbrotli.a" "$SHIM/libbrotlicommon.a"
LIBCTRU_PATH="$ROOT/library/libctru"; LIBBROTLI_PATH="$ROOT/library/libbrotli"; NGHTTP2_PATH="$ROOT/library/nghttp2"; PORTLIBS_PATH="$DEVKITPRO/portlibs/3ds"
INCLUDE_OPTION="-I$PORTLIBS_PATH/include -I$LIBCTRU_PATH/include -I$LIBBROTLI_PATH/include"
LIBRARY_OPTION="-L$PORTLIBS_PATH/lib -L$LIBCTRU_PATH/lib -L$SHIM -L$LIBBROTLI_PATH/lib"
cd "$WORK"
if [ ! -f lib/curl_config.h ]; then
	# identical to library/libcurl/3ds-configure.sh apart from paths; -specs=3dsx.specs only lets configure's
	# link probes succeed on the current toolchain (a static archive is produced, the specs do not enter it)
	./configure CFLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -mword-relocations -ffunction-sections -fdata-sections" \
		CPPFLAGS="-D_3DS -D__3DS__ $INCLUDE_OPTION" LDFLAGS="-specs=3dsx.specs $LIBRARY_OPTION" LIBS="-lctru" \
		--host=arm-none-eabi --disable-shared --enable-static --disable-ipv6 --disable-unix-sockets --disable-manual \
		--disable-ntlm-wb --disable-threaded-resolver --without-ssl --without-zstd --with-mbedtls --with-nghttp2="$NGHTTP2_PATH" \
		> "$OUT/configure.log" 2>&1 || { tail -30 "$OUT/configure.log"; exit 4; }
fi
grep -E "^  (SSL|brotli|HTTP2|zlib|Protocols|Features)" "$OUT/configure.log" | tee "$OUT/CONFIG_SUMMARY" || true
make -C lib -j"$JOBS" libcurl.la > "$OUT/make.log" 2>&1 || { tail -30 "$OUT/make.log"; exit 5; }
cp lib/.libs/libcurl.a "$OUT/lib/libcurl.a"
cp -R include/curl "$OUT/include/"
cp lib/curl_config.h "$OUT/curl_config.h"
echo "curl=$CURL_VER mbedtls=$MBEDTLS_VER devkitARM=$(arm-none-eabi-gcc --version | head -1) date=$(date +%Y-%m-%d)" > "$OUT/BUILD_INFO.txt"
echo "--- public header drift vs bundled library/libcurl/include/curl (empty = identical):"
diff -r "$ROOT/library/libcurl/include/curl" "$OUT/include/curl" | head -20 || true
echo "--- output:"; ls -l "$OUT/lib/libcurl.a"; shasum -a 256 "$OUT/lib/libcurl.a"
