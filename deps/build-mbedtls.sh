#!/bin/sh
#
# GR33N - builds mbedTLS for PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-mbedtls.sh
#
# Leaves $HOME/.gr33n-deps/mbedtls-ps3/ with:
#   include/            headers
#   lib/libmbedtls.a    ONE single library with crypto + x509 + tls
#
# EVERYTHING happens inside $HOME, NEVER in /mnt/c. That's not a matter
# of taste: /mnt/c is DrvFs, the WSL bridge to NTFS, and file
# permissions can't be changed there. The first thing git does on
# clone is write its config and chmod it, so it blows up with
#
#   chmod on .git/config.lock failed: Operation not permitted
#
# And even if that worked, compiling 108 files across that bridge is
# slow in a way you can feel. The project lives on /mnt/c because you
# edit it from Windows; nobody edits the dependencies, so they stay on
# the Linux side.
#
# Why ONE library and not the three mbedTLS generates: the link order
# between libmbedtls, libmbedx509 and libmbedcrypto matters, and
# getting it wrong gives undefined-symbol errors that tell you nothing.
# With a single archive that problem doesn't exist.
#
# Why mbedTLS's own build system isn't used: compiling 108 files and
# putting them into one archive is ten lines you can take in at a
# glance. Fighting somebody else's Makefile with an unusual cross
# toolchain is not ten lines.

set -e

TAG=mbedtls-3.6.4
: "${PS3DEV:=/usr/local/ps3dev}"

CC="$PS3DEV/ppu/bin/ppu-gcc"
AR="$PS3DEV/ppu/bin/ppu-ar"
NM="$PS3DEV/ppu/bin/ppu-nm"

HERE=$(cd "$(dirname "$0")" && pwd)

WORK="${GR33N_DEPS:-$HOME/.gr33n-deps}"
SRC="$WORK/mbedtls-src"
OUT="$WORK/mbedtls-ps3"

mkdir -p "$WORK"

if [ ! -x "$CC" ]; then
	echo "can't find $CC"
	echo "export PS3DEV or fix the path"
	exit 1
fi

# --- sources ----------------------------------------------------------

if [ ! -d "$SRC" ]; then
	echo ">> cloning $TAG"
	git clone --depth 1 --branch "$TAG" \
	          --recurse-submodules --shallow-submodules \
	          https://github.com/Mbed-TLS/mbedtls.git "$SRC"
else
	echo ">> using $SRC (already cloned)"
fi

# --- compilation ------------------------------------------------------

# -DMBEDTLS_USER_CONFIG_FILE is applied AFTER the default configuration:
# it doesn't replace anything, it only strips out what the console
# doesn't have.
CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell"
CFLAGS="$CFLAGS -I$SRC/include -I$HERE"

# And the timer hook.
#
# ps3_mbedtls_config.h sets MBEDTLS_TIMING_ALT, and with that
# include/mbedtls/timing.h does `#include "timing_alt.h"` instead of
# declaring its own structures. That file is ours and lives here.
#
# library/timing.c and library/entropy_poll.c both include it, which
# means it's needed while mbedTLS itself is being compiled, not just
# afterwards.
CFLAGS="$CFLAGS -I$HERE/mbedtls-ps3"

CFLAGS="$CFLAGS -DMBEDTLS_USER_CONFIG_FILE=\"ps3_mbedtls_config.h\""

OBJ="$WORK/.obj-mbedtls"
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

echo ">> compiling with $(basename "$CC")"

n=0
fail=0
for f in "$SRC"/library/*.c; do
	b=$(basename "$f" .c)
	# shellcheck disable=SC2086
	if $CC -c "$f" -o "$OBJ/$b.o" $CFLAGS 2> "$OBJ/$b.err"; then
		n=$((n + 1))
	else
		fail=$((fail + 1))
		echo "   FAILED $b"
		head -5 "$OBJ/$b.err"
	fi
done

echo ">> $n compiled, $fail failed"
[ "$fail" -eq 0 ] || { echo "aborting"; exit 1; }

"$AR" rcs "$OUT/lib/libmbedtls.a" "$OBJ"/*.o
cp -r "$SRC/include/mbedtls" "$SRC/include/psa" "$OUT/include/"
cp "$HERE/ps3_mbedtls_config.h" "$OUT/include/"

# timing_alt.h goes INSIDE include/mbedtls/, next to timing.h.
#
# That's not whimsy: timing.h asks for it with quotes -- `#include
# "timing_alt.h"` --, and a quoted include is looked up FIRST in the
# directory of the file that asks for it. Putting it there means
# anything that uses this install -libpeer, GR33N, the link test- finds
# it without having to add one more -I and without knowing it exists.
cp "$HERE/mbedtls-ps3/timing_alt.h" "$OUT/include/mbedtls/"

# --- what's left unresolved -------------------------------------------
#
# Careful, I got this wrong the first time: nm --undefined-only on an
# ARCHIVE lists the undefined symbols of every object, including the
# ones the object right next to it resolves. It printed 600 and looked
# like a disaster. What you actually have to look at is undefined MINUS
# defined.
#
# What's expected: libc functions (newlib brings those), inet_pton
# (libnet), and the two GR33N adds on purpose. Anything else means the
# library expects something from the operating system that the console
# doesn't have, and it's better to see that now than in the final link
# among a hundred errors that say nothing.

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libmbedtls.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libmbedtls.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	echo
	echo ">> unresolved symbols:"
	comm -23 "$WORK/.undef" "$WORK/.def" | sed 's/^/   /'
	rm -f "$WORK/.undef" "$WORK/.def"

	echo
	echo "   Expected: libc, inet_pton, and mbedtls_hardware_poll +"
	echo "   mbedtls_ms_time, which GR33N writes in net_tls.c."

	# --- AND THAT DTLS-SRTP ACTUALLY MADE IT IN -------------------
	#
	# Compiling without errors does NOT prove the option is set. If
	# MBEDTLS_SSL_DTLS_SRTP failed to land -- a USER_CONFIG_FILE that
	# isn't found, an #undef overriding it -- the 108 files would compile
	# exactly the same: the use_srtp code would simply end up excluded by
	# its #if, without a single warning line.
	#
	# The failure would surface much later, when linking libpeer, as two
	# symbols that don't exist and no clue why.
	#
	# (Nothing needs checking for the timer: if MBEDTLS_TIMING_ALT hadn't
	# landed, library/timing.c would have failed with its own
	# #error "This module only works on Unix and Windows" and we'd never
	# have got this far. There, compiling is the proof.)
	echo
	if "$NM" --defined-only "$OUT/lib/libmbedtls.a" 2>/dev/null \
	   | grep -q "mbedtls_ssl_get_dtls_srtp_negotiation_result"; then
		echo ">> DTLS-SRTP: present (verified in the library, not assumed)"
	else
		echo "!! DTLS-SRTP IS NOT IN THE LIBRARY."
		echo "   MBEDTLS_SSL_DTLS_SRTP didn't make it into the build, and"
		echo "   without it there's nowhere to get the SRTP keys from:"
		echo "   libpeer isn't going to link."
		echo
		echo "   Check that ps3_mbedtls_config.h defines it and that the"
		echo "   -DMBEDTLS_USER_CONFIG_FILE points where it should."
		exit 1
	fi
fi

rm -rf "$OBJ"

echo
echo ">> done: $OUT/lib/libmbedtls.a  ($(du -h "$OUT/lib/libmbedtls.a" | cut -f1))"
echo ">> headers in $OUT/include"
echo
echo "   GR33N's Makefile only looks for it at that path."
echo "   If you move it:  make MBEDTLS=/other/path"
