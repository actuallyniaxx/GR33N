#!/bin/sh
#
# GR33N - builds libSRTP for PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-libsrtp.sh
#
# Leaves $HOME/.gr33n-deps/libsrtp-ps3/ with:
#   include/            headers
#   lib/libsrtp2.a
#
# Same shape as build-mbedtls.sh and for the same reasons: everything inside
# $HOME and never in /mnt/c (DrvFs will not let you change permissions and
# git blows up when cloning), and compiling by hand instead of fighting a
# cross-compiling cmake.
#
# DEPENDS ON mbedTLS. libSRTP can bring its own AES and its own SHA1, but we
# tell it to use mbedTLS's, which is the one that has been working on this
# console for weeks. Dropping in a second, untested AES implementation on
# the platform where byte order has already bitten us makes no sense at all.
# So build-mbedtls.sh first.
#
# WHAT TO LOOK AT WHEN IT FINISHES is the list of unresolved symbols. If
# something shows up there that is not libc or psa_*, libSRTP expects
# something from the system that the console does not have, and it is better
# to see it now than among a hundred link errors that say nothing.
#
# THIS HAS ALREADY BEEN TESTED, though not with ppu-gcc: the 18 files compile
# clean for PowerPC64 big-endian with the Debian cross-compiler, without a
# single warning, not even with -Wcast-align. It is not the same libc nor the
# same system, so new things can turn up here -- but the byte order, which
# was the real risk, is the same and it is checked.
#
# And sure enough one did turn up: the cross-compiler has <netinet/in.h> on
# the default path because it uses glibc, and ppu-gcc does not. See the
# PSL_INC block further down. Two files out of eighteen compiled -- exactly
# the two that do not include datatypes.h.

set -e

: "${PS3DEV:=/usr/local/ps3dev}"

CC="$PS3DEV/ppu/bin/ppu-gcc"
AR="$PS3DEV/ppu/bin/ppu-ar"
NM="$PS3DEV/ppu/bin/ppu-nm"

HERE=$(cd "$(dirname "$0")" && pwd)

WORK="${GR33N_DEPS:-$HOME/.gr33n-deps}"
SRC="$WORK/libsrtp-src"
OUT="$WORK/libsrtp-ps3"
MBED="$WORK/mbedtls-ps3"

mkdir -p "$WORK"

if [ ! -x "$CC" ]; then
	echo "cannot find $CC"
	echo "export PS3DEV or fix the path"
	exit 1
fi

if [ ! -d "$MBED/include" ]; then
	echo "mbedTLS missing from $MBED"
	echo "run this first:  sh build-mbedtls.sh"
	exit 1
fi

# --- sources ----------------------------------------------------------

# THE PINNED COMMIT, AND THIS WAS FOUND OUT LATE.
#
# There used to be a bare `git clone --depth 1` here, i.e. MASTER. And master
# of libSRTP is 3.0.0 today, which is a different API:
# srtp_crypto_policy_set_rtp_default and srtp_crypto_policy_set_rtcp_default
# -- the two that libpeer's dtls_srtp.c calls to set up the crypto policy--
# no longer exist there.
#
# The worst part is how it showed up: the 18 files compiled perfectly and the
# library came out built. 3.0 compiles just as well as 2.x; it just does not
# have those functions. The failure would have surfaced at the very end, when
# linking libpeer, as symbols that do not exist and not a single hint that
# the problem was the VERSION.
#
# 90d05bf is 2.4.2, and it is not our choice: it is the submodule libpeer
# pins in its own tree (third_party/libsrtp of commit 9319aa4). That is, the
# version libpeer is written and tested against.
#
# And it cannot go with --depth 1: that only brings the tip of the branch,
# and this commit is not it.
LIBSRTP_REF=90d05bf8980d16e4ac3f16c19b77e296c4bc207b

if [ ! -d "$SRC" ]; then
	echo ">> cloning libsrtp"
	git clone --quiet https://github.com/cisco/libsrtp.git "$SRC"
fi

echo ">> libsrtp at 2.4.2 ($LIBSRTP_REF)"
(
	cd "$SRC"
	git checkout --quiet "$LIBSRTP_REF" 2>/dev/null || {
		git fetch --quiet origin "$LIBSRTP_REF" && git checkout --quiet "$LIBSRTP_REF"
	}
	git checkout --quiet -- . 2>/dev/null || true
) || {
	echo "!! I could not leave libsrtp at $LIBSRTP_REF."
	echo "   If the earlier clone was --depth 1, delete it and run again:"
	echo "     rm -rf $SRC"
	exit 1
}

# And we check that the version is the one we think it is, instead of assuming.
if ! grep -rq "srtp_crypto_policy_set_rtp_default" "$SRC/include/srtp.h"; then
	echo "!! this libsrtp does not have srtp_crypto_policy_set_rtp_default."
	echo "   It is the API that libpeer's dtls_srtp.c uses. If this fires,"
	echo "   the checkout has not landed on 2.4.2."
	exit 1
fi

# --- the config.h ------------------------------------------------------
#
# libSRTP expects it from autoconf or cmake. Here it goes in by hand, and
# inside it is WORDS_BIGENDIAN, the one line everything working depends on.
# See the long comment in ps3_srtp_config.h.

mkdir -p "$WORK/.srtp-cfg"
cp "$HERE/ps3_srtp_config.h" "$WORK/.srtp-cfg/config.h"

# --- compilation ------------------------------------------------------

# THE PSL1GHT HEADERS ARE NOT ON ppu-gcc's DEFAULT PATH.
#
# libSRTP includes <netinet/in.h> for ntohs, and that header DOES exist --
# the link.c in GR33N has been using it for weeks. The thing is it lives in
# the PSL1GHT directory, and what puts it on the path is $(LIBPSL1GHT_INC)
# from ppu_rules, i.e. the project Makefile. Compiling by hand, like here,
# nobody puts it there.
#
# We look for it instead of taking it as known: if PSL1GHT ever moves, this
# says so instead of failing with sixteen identical errors.
PSL_INC=""
for d in "$PS3DEV/ppu/include" "$PSL1GHT/ppu/include" \
         "$PS3DEV/portlibs/ppu/include"; do
	if [ -f "$d/netinet/in.h" ]; then
		PSL_INC="-I$d"
		echo ">> PSL1GHT headers in $d"
		break
	fi
done

if [ -z "$PSL_INC" ]; then
	echo "cannot find netinet/in.h on any PSL1GHT path."
	echo "Looked in:"
	echo "   $PS3DEV/ppu/include"
	echo "   $PSL1GHT/ppu/include"
	echo "   $PS3DEV/portlibs/ppu/include"
	echo
	echo "Find where it is with:  find \$PS3DEV -name in.h -path '*netinet*'"
	exit 1
fi

CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell -DHAVE_CONFIG_H"
CFLAGS="$CFLAGS -I$WORK/.srtp-cfg"
CFLAGS="$CFLAGS -I$SRC/include -I$SRC/crypto/include -I$SRC"
CFLAGS="$CFLAGS -I$MBED/include"

# THE SAME mbedTLS CONFIGURATION mbedTLS ITSELF WAS COMPILED WITH.
#
# This was missing, and it went unnoticed because libSRTP got lucky: it only
# uses mbedtls_aes_*, mbedtls_gcm_* and mbedtls_md_*, which are on by
# default. But luck is not a design.
#
# Without this -D, the mbedTLS headers are read in their DEFAULT state, which
# is not the state libmbedtls.a was built with. And the mbedTLS configuration
# does not only switch functions on and off: there are struct fields guarded
# by #if. Compiling against one configuration and linking against another
# gives different struct sizes on each side, and that is silent memory
# corruption -- exactly the usrsctp sockaddr_conn bug, through another door.
#
# In libpeer it did get noticed, and loudly: dtls_srtp.c could not find
# mbedtls_ssl_srtp_profile nor any of the SRTP constants, because
# MBEDTLS_SSL_DTLS_SRTP is switched on by THIS file. The symptom was noisy
# there; here it would have been mute.
#
# ps3_mbedtls_config.h is copied into $MBED/include by build-mbedtls.sh, so
# the -I above already puts it on the path.
CFLAGS="$CFLAGS -DMBEDTLS_USER_CONFIG_FILE=\"ps3_mbedtls_config.h\""

CFLAGS="$CFLAGS $PSL_INC"

# Strict aliasing OFF. libSRTP reads 32-bit words out of char buffers in
# several places, which is exactly what GCC takes to be impossible at -O2. On
# x86 the generated code comes out right by luck; here we are not going to
# depend on luck.
CFLAGS="$CFLAGS -fno-strict-aliasing"

# And let it warn if some cast leaves a pointer worse aligned than it promises.
CFLAGS="$CFLAGS -Wcast-align"

# The files, in the same selection CMakeLists.txt makes with ENABLE_MBEDTLS:
# the crypto comes from mbedTLS, not from libSRTP itself.
# aes.c, aes_icm.c, hmac.c and sha1.c are left out on purpose.
# srtp/srtp_policy.c IS NOT HERE: it is from 3.0, which split srtp.c in two.
# In 2.4.2 -- which is what libpeer pins and therefore what we compile-- all
# of that lives inside srtp/srtp.c.
#
# What does NOT go in, and it is on purpose: aes.c, aes_icm.c, hmac.c and
# sha1.c are libSRTP's own crypto, and here mbedTLS provides it (MBEDTLS 1 in
# ps3_srtp_config.h). The _nss and _ossl files are the other two alternatives.
FILES="
srtp/srtp.c
crypto/cipher/cipher.c
crypto/cipher/cipher_test_cases.c
crypto/cipher/null_cipher.c
crypto/cipher/aes_icm_mbedtls.c
crypto/cipher/aes_gcm_mbedtls.c
crypto/hash/auth.c
crypto/hash/auth_test_cases.c
crypto/hash/null_auth.c
crypto/hash/hmac_mbedtls.c
crypto/kernel/alloc.c
crypto/kernel/crypto_kernel.c
crypto/kernel/err.c
crypto/kernel/key.c
crypto/math/datatypes.c
crypto/replay/rdb.c
crypto/replay/rdbx.c
"

OBJ="$WORK/.obj-srtp"
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

echo ">> compiling with $(basename "$CC")"

n=0
fail=0
for f in $FILES; do
	b=$(echo "$f" | tr '/' '_' | sed 's/\.c$//')
	if [ ! -f "$SRC/$f" ]; then
		echo "   MISSING $f  (the list above has fallen behind this"
		echo "                version of libSRTP)"
		fail=$((fail + 1))
		continue
	fi
	# shellcheck disable=SC2086
	if $CC -c "$SRC/$f" -o "$OBJ/$b.o" $CFLAGS 2> "$OBJ/$b.err"; then
		n=$((n + 1))
		# Warnings are shown even when it compiles: -Wcast-align on this
		# platform is information, not noise.
		if [ -s "$OBJ/$b.err" ]; then
			echo "   warnings in $f:"
			head -6 "$OBJ/$b.err" | sed 's/^/     /'
		fi
	else
		fail=$((fail + 1))
		echo "   FAILED $f"
		head -8 "$OBJ/$b.err" | sed 's/^/     /'
	fi
done

echo ">> $n compiled, $fail failed"
[ "$fail" -eq 0 ] || { echo "aborting"; exit 1; }

"$AR" rcs "$OUT/lib/libsrtp2.a" "$OBJ"/*.o
cp -r "$SRC/include/"*.h "$OUT/include/"
cp "$HERE/ps3_srtp_config.h" "$OUT/include/"

# AND THE SAME ONES AGAIN, INSIDE srtp2/.
#
# libpeer includes them like this: `#include <srtp2/srtp.h>`. That
# subdirectory is how libSRTP really gets installed -- its own `make install`
# does it, and it is where everyone who uses it looks-- but here we compile
# by hand and nobody was creating it. It came out as "srtp2/srtp.h: No such
# file or directory" in four libpeer files.
#
# They are copied to both places instead of moved: anyone including a bare
# <srtp.h> -- like libSRTP itself does internally-- still finds it.
mkdir -p "$OUT/include/srtp2"
cp "$SRC/include/"*.h "$OUT/include/srtp2/"

# --- what is left unresolved ------------------------------------------
#
# Undefined MINUS defined, not undefined on its own: nm on an ARCHIVE lists
# the undefined symbols of every object, including the ones the object next
# to it resolves. We already paid for that lesson with mbedTLS.

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libsrtp2.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libsrtp2.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	echo
	echo ">> unresolved symbols:"
	comm -23 "$WORK/.undef" "$WORK/.def" | sed 's/^/   /'
	rm -f "$WORK/.undef" "$WORK/.def"

	echo
	echo "   Expected:"
	echo "     - libc: memcpy, memset, calloc, free, strlen, rand, clock,"
	echo "       exit, sscanf, vfprintf... newlib brings all of that."
	echo "     - mbedtls_aes_*, mbedtls_gcm_* and mbedtls_md_*, which turn"
	echo "       up when linking the EBOOT against libmbedtls.a."
	echo
	echo "   CAREFUL, THIS CHANGED WHEN 2.4.2 WAS PINNED. This used to say"
	echo "   thirteen psa_* symbols would show up, and that was true with"
	echo "   3.0.0: that one talks to mbedTLS through the new API (PSA)."
	echo "   2.4.2 uses the CLASSIC one -- mbedtls_aes_crypt_ctr,"
	echo "   mbedtls_gcm_setkey, mbedtls_md_hmac_starts--, which is a"
	echo "   whole other list of symbols. The message was left describing"
	echo "   the version we no longer compile."
	echo
	echo "   Anything ELSE has to be looked at before going on."

	# --- AND THAT mbedTLS BRINGS THOSE SYMBOLS, CHECKED -----------
	#
	# That they come out unresolved here is normal: they get resolved
	# when linking the EBOOT. What is NOT normal is taking it for
	# granted. If ps3_mbedtls_config.h switched off MBEDTLS_GCM_C or
	# MBEDTLS_MD_C -- it does not today, but that is exactly the kind
	# of trimming somebody does one day to save space-- these would
	# stay unresolved until the very last link, among another hundred
	# errors.
	#
	# The check costs one nm and it happens now.
	MBED_A="$MBED/lib/libmbedtls.a"
	if [ -f "$MBED_A" ]; then
		echo
		sin=""
		for s in mbedtls_aes_crypt_ctr mbedtls_gcm_setkey \
		         mbedtls_md_hmac_starts mbedtls_md_info_from_type; do
			if ! "$NM" --defined-only "$MBED_A" 2>/dev/null \
			     | grep -q " $s\$"; then
				sin="$sin $s"
			fi
		done

		if [ -z "$sin" ]; then
			echo ">> the classic mbedTLS API is in libmbedtls.a"
			echo "   (aes, gcm and md: checked, not assumed)"
		else
			echo "!! libmbedtls.a does NOT define:$sin"
			echo
			echo "   libSRTP 2.4.2 needs them. Check that"
			echo "   ps3_mbedtls_config.h has not switched off"
			echo "   MBEDTLS_AES_C, MBEDTLS_GCM_C or MBEDTLS_MD_C."
			exit 1
		fi
	fi
fi

rm -rf "$OBJ"

echo
echo ">> done: $OUT/lib/libsrtp2.a  ($(du -h "$OUT/lib/libsrtp2.a" | cut -f1))"
echo ">> headers in $OUT/include"
