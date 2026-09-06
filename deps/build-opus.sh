#!/bin/sh
#
# GR33N - builds libopus for PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-opus.sh
#
# Leaves $HOME/.gr33n-deps/opus-ps3/ with:
#   include/opus/       headers
#   lib/libopus.a
#
# Same shape as build-mbedtls.sh and build-libsrtp.sh, and for the same
# reasons: everything inside $HOME and never in /mnt/c (DrvFs will not let
# you change permissions), and a provisional output that only moves into
# place once every check passes -- because that one already bit us, when a
# failed build-libpeer.sh deleted peer.h and the next make died with an
# error that had nothing to do with it.
#
# ------------------------------------------------------------------
# WHY OPUS AND NOT SOMETHING ELSE
# ------------------------------------------------------------------
#
# Because it is what xCloud sends and there is no choice. The SDP offer
# that already comes out of the console says:
#
#   m=audio 9 UDP/TLS/RTP/SAVPF 111
#   a=rtpmap:111 opus/48000/2
#
# Which is to say we have spent weeks receiving Opus audio and throwing it
# away for want of anything to decode it with.
#
# ------------------------------------------------------------------
# FIXED POINT, AND IT IS NOT A SILLY PRECAUTION
# ------------------------------------------------------------------
#
# --enable-fixed-point. The Cell's PPE is an in-order core from 2006 with
# an unremarkable floating-point unit, and on top of that it is already
# carrying the WebRTC packet pumping, the SRTP decryption and the hand-off
# to the RSX. The SPU is busy with H.264.
#
# The fixed-point build of Opus is the one phones use: it is not an odd or
# little-tested path, it is the more tested of the two. The difference in
# quality against floating point is inaudible -- the Opus people say so
# themselves-- and the cost is far more predictable, which on a console
# where we are already tight matters more than the last decibel.
#
# If one day there is CPU to spare, take out the --enable-fixed-point and
# measure. But measure: you do not change it on a hunch.
#
# ------------------------------------------------------------------
# NO RESAMPLING NEEDED, AND THAT IS A GIFT
# ------------------------------------------------------------------
#
# Opus decodes at 48000 Hz. The PS3's audio port puts out 48000 Hz. They
# are the same number, so there is no resampling, no filter, no error
# piling up and no third library to compile.
#
# It is one of the few things in this port that come for free. Best not to
# spoil it by dropping in a resampler "just in case".

set -e

: "${PS3DEV:=/usr/local/ps3dev}"

CC="$PS3DEV/ppu/bin/ppu-gcc"
AR="$PS3DEV/ppu/bin/ppu-ar"
NM="$PS3DEV/ppu/bin/ppu-nm"
RANLIB="$PS3DEV/ppu/bin/ppu-ranlib"

HERE=$(cd "$(dirname "$0")" && pwd)

WORK="${GR33N_DEPS:-$HOME/.gr33n-deps}"
SRC="$WORK/opus-src"
OUT="$WORK/opus-ps3"
OBRAS="$WORK/.opus-ps3-en-obras"

mkdir -p "$WORK"

if [ ! -x "$CC" ]; then
	echo "cannot find $CC"
	echo "export PS3DEV or fix the path"
	exit 1
fi

# --- sources ----------------------------------------------------------

# THE PINNED COMMIT, for the same reason as in libsrtp: master is not a
# version, it is whatever happens to be there today.
#
# v1.4, not "v1.4.3": that version DOES NOT EXIST, I made it up. The ones
# xiph/opus has are v1.3.1, v1.4, v1.5, v1.5.1, v1.5.2. Now the script
# checks instead of trusting, and if the tag is not there it shows you the
# ones that are.
#
# 1.4 and not 1.5 because 1.5 brings DRED, a neural network model for
# hiding losses. On a PPE from 2006 that has no business being there.
OPUS_REF=v1.4

if [ ! -d "$SRC" ]; then
	echo ">> cloning opus"
	git clone --quiet https://github.com/xiph/opus.git "$SRC"
fi

echo ">> opus at $OPUS_REF"

# THE PARENTHESES THAT SWALLOWED THE FAILURE.
#
# There was a subshell here that ended in `git checkout -- . || true`. The
# exit status of a subshell is that of its LAST command, and that last
# command was literally incapable of failing. So the `|| exit 1` on the
# outside never fired once.
#
# Result: "error: pathspec 'v1.4.3' did not match any file(s)" went up on
# screen, the script said ">> gathering sources" and carried on quite
# happily compiling MASTER. The qext_cache and NB_QEXT_BANDS errors that
# came afterwards were exactly that: 1.5 code that should never have been
# there.
#
# A guard that cannot fail is not a guard. That makes three in this
# project, and I wrote this one myself half an hour ago.
if ! (
	cd "$SRC" &&
	{ git checkout --quiet "$OPUS_REF" 2>/dev/null ||
	  { git fetch --quiet --tags origin &&
	    git checkout --quiet "$OPUS_REF"; }; } &&
	git checkout --quiet -- .
); then
	echo "!! the tag $OPUS_REF does not exist in this clone."
	echo
	echo "   The ones that do:"
	(cd "$SRC" && git tag -l 'v*' | tail -12 | sed 's/^/     /')
	echo
	echo "   If the clone was --depth 1, delete it and try again:  rm -rf $SRC"
	exit 1
fi

# --- and NOW we really do check which version it is -------------------
#
# The check that used to be here looked for opus_decode_float in opus.h.
# That function has existed since 1.0: the check passed with ANY version,
# including the master we did not want. Checking something that is always
# true is checking nothing at all.
#
# These two do tell them apart:

VER=$( cd "$SRC" && git describe --tags --exact-match HEAD 2>/dev/null || echo "(none)" )
if [ "$VER" != "$OPUS_REF" ]; then
	echo "!! HEAD is at '$VER', not at '$OPUS_REF'."
	exit 1
fi
echo "   git describe says: $VER"

# NB_QEXT_BANDS only exists from 1.5 onwards. If it shows up, this is
# master however much the tag says otherwise.
if grep -rq "NB_QEXT_BANDS" "$SRC/celt/" 2>/dev/null; then
	echo "!! this tree has QEXT: it is 1.5 or master, not 1.4."
	echo "   Clean it out and try again:  rm -rf $SRC"
	exit 1
fi
echo "   no QEXT: this is the 1.4 branch"

# --- configuration by hand --------------------------------------------
#
# NO ./configure. Opus ships autotools and cmake, and both of them want to
# compile and RUN test programs to work things out about the system.
# Cross-compiling to PowerPC you cannot do that: the binary does not run
# here. You can either fight with --host and a cache of canned answers, or
# you can write config.h by hand, which is twenty lines and does not lie.
#
# It is exactly what was already done with mbedTLS and with libSRTP, and
# for the same reason.

mkdir -p "$WORK/.opus-cfg"
cat > "$WORK/.opus-cfg/config.h" <<'CFG'
/* GR33N - opus config.h for PS3, written by hand.
 *
 * ------------------------------------------------------------------
 * I NEARLY CAME A CROPPER HERE, AND THE FAILURE WOULD HAVE BEEN SILENT
 * ------------------------------------------------------------------
 *
 * The first version of this file put things like:
 *
 *     #define DISABLE_FLOAT_API  0
 *     #define HAVE_LRINTF        0
 *     #define USE_ALLOCA         0
 *
 * reading "0" as "off". But opus does NOT test them with #if, it tests
 * them with #ifdef:
 *
 *     #ifndef DISABLE_FLOAT_API      <- in opus.h, around
 *     ...                               opus_decode_float
 *     #endif
 *
 * Defining something to 0 leaves it DEFINED. Which means those three
 * lines meant exactly the opposite of what they looked like:
 *
 *   - DISABLE_FLOAT_API 0  ->  opus_decode_float goes away, which is the
 *                              very function aud.c needs. The check at the
 *                              end of the script would have caught it, but
 *                              with a "not there" and no word on why.
 *   - HAVE_LRINTF 0        ->  opus calls lrintf, which newlib may not
 *                              ship. Link error right at the very end.
 *   - USE_ALLOCA 0         ->  alloca() in threads with 64 KB stacks.
 *                              That does not error: it tramples memory.
 *
 * The rule, so as not to repeat it: nothing here is turned off by setting
 * it to zero. What you do not want, YOU DO NOT WRITE.
 */
#ifndef GR33N_OPUS_CONFIG_H
#define GR33N_OPUS_CONFIG_H

#define OPUS_BUILD       1
#define PACKAGE_VERSION  "1.4-gr33n"

/* FIXED POINT. See the long comment in build-opus.sh. */
#define FIXED_POINT      1

/* C99 variable-length arrays instead of alloca(). The threads in this
 * project have measured stacks and alloca respects none of them. */
#define VAR_ARRAYS       1

/* AND NOTHING ELSE.
 *
 * In particular these are NOT here, on purpose and not by oversight:
 *   DISABLE_FLOAT_API   - we want the float API (the PS3 port eats
 *                         float; converting here saves a step)
 *   HAVE_LRINTF / HAVE_LRINT - newlib does not guarantee them
 *   USE_ALLOCA          - see above
 *   OPUS_HAVE_RTCD      - the PPE has VMX, but the opus PowerPC path is
 *                         neither written nor tested for an in-order
 *                         core. Plain C is correct, and here correctness
 *                         is worth more. If one day there is CPU to
 *                         spare, this is the first thing to measure.
 */

#endif
CFG

# --- which sources ----------------------------------------------------
#
# WE ASK OPUS. They are not listed by hand and they are not hunted down
# with find either: opus ships its own lists in celt_sources.mk,
# silk_sources.mk and opus_sources.mk, which is what its real Makefile.am
# uses.
#
# The previous attempt was a find with exclusions (-not -path '*/arm/*',
# '*/x86/*'...) and it left out celt/dump_modes/, which is a build tool
# and not library. Hence the dump_modes.c errors.
#
# A list of exclusions is a parallel list maintained by hand through the
# back door: you have to remember every new directory that turns up. The
# .mk files are the real list, opus maintains them, and on top of that
# they separate fixed point from floating without us having to decide it.
#
# They are read with make, which is what knows how to read them. Writing a
# Makefile parser by hand for this would be the third list.

echo ">> asking opus which files are its own"

cat > "$WORK/.opus-cfg/listar.mk" <<'MK'
include celt_sources.mk
include silk_sources.mk
include opus_sources.mk

listar:
	@echo $(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FIXED) \
	      $(OPUS_SOURCES) $(OPUS_SOURCES_FLOAT)
MK

FUENTES_REL=$(make -s -C "$SRC" -f "$WORK/.opus-cfg/listar.mk" listar) || {
	echo "!! I could not read opus's .mk lists."
	echo "   See if they are there:  ls $SRC/*_sources.mk"
	exit 1
}

# To absolute paths, and checking that each one exists. A file that the
# .mk names and is not there is a sign of an incomplete tree, and it is
# worth seeing that here rather than as an unresolved symbol half an hour
# from now.
FUENTES=""
N=0
for f in $FUENTES_REL; do
	case "$f" in
		*.c) ;;
		*) continue ;;
	esac

	if [ ! -f "$SRC/$f" ]; then
		echo "!! the list names $f and it is not in the tree"
		exit 1
	fi

	FUENTES="$FUENTES $SRC/$f"
	N=$((N + 1))
done

echo "   $N files, from opus's own lists"

# NOTHING IS CHECKED AGAINST A NUMBER, and this one already bit me: the
# first version demanded "at least 150 files" -- a threshold I pulled out
# of thin air by comparing it with the 137 the broken find had gathered
# before. The 1.4 lists give 131, which is THE RIGHT ANSWER, and the check
# knocked back a good result.
#
# An invented threshold checks nothing: it only says whether the number
# looks like the one you were expecting. What really matters is that all
# FOUR lists contributed something; if a variable does not expand, its
# whole group disappears and that is a real failure. It is looked at by
# prefix, which does not depend on me remembering any filename correctly.

falta_grupo=0
for pre in "celt/" "silk/" "silk/fixed/" "src/"; do
	if ! echo "$FUENTES_REL" | tr " " "\n" | grep -q "^$pre"; then
		echo "!! not one file from $pre: that list did not expand"
		falta_grupo=1
	fi
done

# And two specific files, checked one by one against the v1.4 tree before
# writing them in here. If these two are present, the lists have been
# read.
for f in silk/decode_core.c silk/fixed/find_LPC_FIX.c; do
	if ! echo "$FUENTES_REL" | tr " " "\n" | grep -qx "$f"; then
		echo "!! $f is missing from the list"
		falta_grupo=1
	fi
done

if [ "$falta_grupo" = "1" ]; then
	echo
	echo "   Try it by hand:"
	echo "     make -s -C $SRC -f $WORK/.opus-cfg/listar.mk listar"
	exit 1
fi

echo "   all four lists contributed files"

# --- the PSL1GHT headers ----------------------------------------------
#
# Same as in libsrtp: they are not on ppu-gcc's default path, the
# project's Makefile puts them there from ppu_rules. Compiling by hand
# nobody puts them there. Opus barely uses system headers, but <stdint.h>
# and friends it does, and it is better to have the right path.

PSL_INC=""
for d in "$PS3DEV/ppu/include" "$PSL1GHT/ppu/include" \
         "$PS3DEV/portlibs/ppu/include"; do
	if [ -f "$d/netinet/in.h" ]; then
		PSL_INC="-I$d"
		echo ">> PSL1GHT headers at $d"
		break
	fi
done

[ -n "$PSL_INC" ] || echo ">> no PSL1GHT headers (opus barely uses them)"

# --- compilation ------------------------------------------------------

CFLAGS="-O2 -std=gnu99 -mcpu=cell -DHAVE_CONFIG_H"
CFLAGS="$CFLAGS -I$WORK/.opus-cfg"
CFLAGS="$CFLAGS -I$SRC/include -I$SRC/celt -I$SRC/silk -I$SRC/silk/fixed"
CFLAGS="$CFLAGS -I$SRC/src"
CFLAGS="$CFLAGS $PSL_INC"

# Strict aliasing OFF, for the same reason as in libSRTP: Opus reads words
# out of char buffers in the range encoder. On x86 that comes out right by
# luck.
CFLAGS="$CFLAGS -fno-strict-aliasing"

rm -rf "$OBRAS"
mkdir -p "$OBRAS/obj" "$OBRAS/lib" "$OBRAS/include/opus"

echo ">> compiling ($N files, this takes a while)"

FALLOS=0
i=0
for f in $FUENTES; do
	i=$((i + 1))
	o="$OBRAS/obj/$(echo "$f" | sed "s|$SRC/||; s|/|_|g; s|\.c$|.o|")"

	if ! $CC $CFLAGS -c "$f" -o "$o" 2> "$OBRAS/err.txt"; then
		echo
		echo "!! fails: $f"
		sed 's/^/   /' "$OBRAS/err.txt" | head -20
		FALLOS=$((FALLOS + 1))
		[ "$FALLOS" -ge 3 ] && {
			echo
			echo "   three failures, stopping. The ones above are usually"
			echo "   the same problem repeated."
			exit 1
		}
	fi

	[ $((i % 40)) -eq 0 ] && echo "   $i/$N"
done

[ "$FALLOS" -eq 0 ] || { echo "!! $FALLOS files do not compile"; exit 1; }

echo "   $N/$N"

# --- library ----------------------------------------------------------

echo ">> archiving"
$AR rcs "$OBRAS/lib/libopus.a" "$OBRAS"/obj/*.o
$RANLIB "$OBRAS/lib/libopus.a" 2>/dev/null || true

cp "$SRC/include/"*.h "$OBRAS/include/opus/"

# --- the checks -------------------------------------------------------
#
# That it compiles does not mean it is any use. What is needed is that the
# functions aud.c is going to call are there, and that nothing from the
# system is missing.

echo
echo "=============================================================="
echo " checks"
echo "=============================================================="

# A FUNCTION ON POWERPC64 HAS TWO SYMBOLS, AND THAT KNOCKED BACK ALL NINE.
#
# The PPU uses the ELFv1 ABI, where a function is not an address: it is a
# three-word DESCRIPTOR that lives in the .opd section and points at the
# code. So nm shows two entries per function:
#
#     0000000000000000 D opus_decoder_create      <- the descriptor
#     0000000000000000 T .opus_decoder_create     <- the code, with a dot
#
# The old check demanded " T opus_decoder_create" and said NO on all nine,
# with libopus.a perfectly well built. A "not there" that really said
# "not there the way I expected".
#
# The funny thing is that the audio probe DID do it right --it filtered on
# " T | D | B "-- and had it in front of it in its own output: audioInit
# came out next to __audioInit. It was written down and I did not look.
#
# Now the NAME is compared, with or without a leading dot, and if it fails
# it shows what nm really says instead of leaving it at a bare NO.
falta=0
for s in opus_decoder_create opus_decoder_destroy opus_decode \
         opus_decode_float opus_decoder_ctl opus_packet_get_nb_channels \
         opus_packet_get_nb_frames opus_packet_get_samples_per_frame \
         opus_strerror; do
	if $NM --defined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
	   awk '{print $NF}' | grep -Eqx "\.?$s"; then
		echo "   $s ... YES"
	else
		echo "   $s ... NO  <<<<"
		echo "      what nm says about that name:"
		$NM --defined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
			grep -- "$s" | head -4 | sed 's/^/        /'
		falta=1
	fi
done

if [ "$falta" = "1" ]; then
	echo
	echo "!! functions aud.c needs are missing. NOT installing."
	echo "   The half-built library stays in $OBRAS so you can look at it."
	exit 1
fi

echo
echo " UNRESOLVED symbols that are not from libc:"
echo " (if something odd shows up here, opus expects something from the"
echo "  system that the console does not have, and it is better to see it"
echo "  now than among a hundred link errors that say nothing)"
# $NF and not $2, and stripping the leading dot: for the same reason as
# above, in ELFv1 a function symbol can come through as ".foo". And out go
# the archive member header lines, which end in ":".
$NM --undefined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
	awk '{ n = $NF; sub(/^\./, "", n); print n }' | sort -u |
	grep -v '^$' | grep -v ':$' |
	grep -Ev '^(memcpy|memset|memmove|malloc|calloc|free|realloc|abort|exit)$' |
	grep -Ev '^(printf|fprintf|sprintf|snprintf|puts|fputs|fwrite|stderr)$' |
	grep -Ev '^(sqrt|exp|log|pow|floor|ceil|fabs|cos|sin|atan|atan2)f?$' |
	grep -Ev '^(celt_|silk_|opus_|_?_?ec_|clt_|comb_|deemphasis|resampler)' |
	sed 's/^/   /'

# And that fixed point really is switched on. If FIXED_POINT had not got
# through, opus would compile just the same in floating point and nobody
# would find out until measuring the cost on the console.
echo
if $NM --defined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
   grep -q "silk_.*_FIX"; then
	echo " fixed point ... YES (there are silk_*_FIX symbols)"
else
	echo " fixed point ... NO  <<<<"
	echo "   FIXED_POINT has not reached the compiler. It compiles just"
	echo "   the same, in float, and costs more. Check the -I of config.h."
fi

# --- install ----------------------------------------------------------

rm -rf "$OUT"
mv "$OBRAS" "$OUT"
rm -f "$OUT/err.txt"
rm -rf "$OUT/obj"

echo
echo "=============================================================="
echo " done:  $OUT"
echo "   $OUT/lib/libopus.a       $(du -h "$OUT/lib/libopus.a" | cut -f1)"
echo "   $OUT/include/opus/"
echo
echo " The Makefile should already be pointing there. If not:"
echo "   LIBDIRS += -L$OUT/lib"
echo "   INCLUDE += -I$OUT/include"
echo "   LIBS    += -lopus"
echo "=============================================================="
