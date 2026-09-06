#!/bin/sh
#
# GR33N - builds libpeer for PS3 (PSL1GHT / ppu-gcc)
#
#   sh deps/build-libpeer.sh
#
# Leaves $HOME/.gr33n-deps/libpeer-ps3/ with:
#   include/peer.h, peer_connection.h
#   lib/libpeer.a
#
# THIS IS THE LAST OF THE FOUR. The others have to be done first, in this order:
#
#   sh deps/build-mbedtls.sh    (108 files, with DTLS-SRTP inside)
#   sh deps/build-libsrtp.sh    (18 files)
#   sh deps/build-usrsctp.sh    (23 files)
#
# WHAT libpeer IS AND WHAT IT ISN'T. It is 6653 lines that glue together mbedTLS
# (DTLS), libSRTP (the encryption of the RTP packets), usrsctp (the data
# channels) and an ICE agent of its own. It doesn't decode video or reassemble
# anything: that's GR33N's job.

set -e

: "${PS3DEV:=/usr/local/ps3dev}"

CC="$PS3DEV/ppu/bin/ppu-gcc"
AR="$PS3DEV/ppu/bin/ppu-ar"
NM="$PS3DEV/ppu/bin/ppu-nm"

HERE=$(cd "$(dirname "$0")" && pwd)
SHIM="$HERE/ps3-shim"

WORK="${GR33N_DEPS:-$HOME/.gr33n-deps}"
SRC="$WORK/libpeer-src"
OUT="$WORK/libpeer-ps3"

MBED="$WORK/mbedtls-ps3"
# libsrtp-ps3, with "lib" in front: that's what build-libsrtp.sh calls it. This
# used to say srtp-ps3 and the script would stop dead saying "run
# build-libsrtp.sh first" right after build-libsrtp.sh had finished fine.
SRTP="$WORK/libsrtp-ps3"
SCTP="$WORK/usrsctp-ps3"

# THE COMMIT, AND IT ISN'T A WHIM.
#
# green-nx left it pinned with this warning: "upstream moves under us (2026-08
# master broke every hunk of libpeer-switch.patch and bumped mbedtls to the
# 4.x layout)". Our patch comes out of theirs, so it inherits the pin. Moving
# it up means rebasing 41 hunks and testing a real stream all over again.
LIBPEER_REF=9319aa434cb9e893faed0293ba9d2a21eca59c8b

mkdir -p "$WORK"

if [ ! -x "$CC" ]; then
	echo "can't find $CC"
	exit 1
fi

# --- the three before this one -----------------------------------------

for d in "$MBED" "$SRTP" "$SCTP"; do
	if [ ! -d "$d/include" ]; then
		echo "missing $d"
		echo
		echo "libpeer needs the other three done first:"
		echo "   sh deps/build-mbedtls.sh"
		echo "   sh deps/build-libsrtp.sh"
		echo "   sh deps/build-usrsctp.sh"
		exit 1
	fi
done

# And that mbedTLS really does bring DTLS-SRTP. Without it, libpeer's
# dtls_srtp.c is left without the two functions the SRTP keys come out of, and
# the error would only show up at link time without saying why.
if [ -x "$NM" ] && [ -f "$MBED/lib/libmbedtls.a" ]; then
	if ! "$NM" --defined-only "$MBED/lib/libmbedtls.a" 2>/dev/null \
	     | grep -q "mbedtls_ssl_get_dtls_srtp_negotiation_result"; then
		echo "!! the libmbedtls.a that's there does NOT have DTLS-SRTP."
		echo "   Run deps/build-mbedtls.sh again; it now checks for that itself."
		exit 1
	fi
fi

# --- the PSL1GHT headers -----------------------------------------------

PSL_INC=""
PSL_LIB=""
for d in "$PS3DEV/ppu" "$PSL1GHT/ppu" "$PS3DEV/portlibs/ppu"; do
	if [ -f "$d/include/netinet/in.h" ]; then
		PSL_INC="-I$d/include"
		PSL_LIB="-L$d/lib"
		echo ">> PSL1GHT headers in $d/include"
		break
	fi
done

if [ -z "$PSL_INC" ]; then
	echo "can't find netinet/in.h in any PSL1GHT path"
	exit 1
fi

PROBE="$WORK/.probe-libpeer"
mkdir -p "$PROBE"

# --- THE PROBE ---------------------------------------------------------
#
# IT LINKS, it doesn't just compile. That's the lesson from the CMSG round: in
# C99, calling something that isn't declared is a WARNING, so "it compiles"
# doesn't mean "it exists". And one thing per test: the first version of the
# socket probe put select and FD_ISSET in the same program, it failed, and
# there was no way to tell which of the two was missing.

echo
echo ">> checking what the console has"

LIBS_SONDA="-lnet -lnetctl -lsysmodule -lrt -llv2 -lm"
OPTS_SONDA="-std=gnu99 $PSL_INC -Werror=implicit-function-declaration -Werror=implicit-int"

enlaza() {
	printf '%s\n' "$2" > "$PROBE/p.c"
	# shellcheck disable=SC2086
	if $CC "$PROBE/p.c" -o "$PROBE/p.elf" $OPTS_SONDA $PSL_LIB $LIBS_SONDA \
	   > "$PROBE/p.err" 2>&1; then
		echo "   YES  $1"
		return 0
	else
		echo "   no   $1"
		return 1
	fi
}

# select IS PROBED, BUT IT ISN'T USED ANY MORE. And it deserves a warning
# line, because this "YES" is exactly the one that threw us off for two
# rounds: select EXISTS in PSL1GHT and links without complaining. What you
# can't do is call it, because PS3 descriptors are worth 0x40000026 and FD_SET
# turns them into a write 64 MB off the end of the stack.
#
# The probe stays because the day PSL1GHT fixes it we'll want to see that,
# but the patch sends all three libpeer sites through netPoll.
SELECT_DEF=""
if ! enlaza "select()" '#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
int main(void){fd_set r; struct timeval tv;
FD_ZERO(&r); tv.tv_sec=0; tv.tv_usec=1000;
return select(1,&r,0,0,&tv);}'; then
	SELECT_DEF="-DGR33N_FALTA_SELECT=1"
	echo "        -> ps3_net_stubs.c supplies it on top of netSelect()"
else
	echo "        (it exists, but it makes no odds: on the PS3 you can't"
	echo "         call it. libpeer goes by netPoll. See agent.c in the patch.)"
fi

ISSET_DEF=""
if ! enlaza "FD_ISSET" '#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
int main(void){fd_set r; FD_ZERO(&r); return FD_ISSET(0,&r) ? 1 : 0;}'; then
	ISSET_DEF="-DGR33N_FALTA_FD_ISSET=1"
	echo "        !! FD_ISSET IS NOT THERE, and libpeer uses it in agent.c:101."
	echo "           That one isn't solved yet: paste this output."
	exit 1
fi

# Byte order, which is what reading a single RTP packet properly depends on.
if ! enlaza "<endian.h> with __BYTE_ORDER" "#include <endian.h>
#if !defined(__BYTE_ORDER) || !defined(__BIG_ENDIAN)
#error faltan
#endif
#if __BYTE_ORDER != __BIG_ENDIAN
#error no es big-endian
#endif
int main(void){return 0;}"; then
	echo "        -> ps3-shim/endian.h supplies it (so -I\$SHIM goes first)"
fi

# --- THE net* FAMILY ---------------------------------------------------
#
# The patch moves ALL of socket.c over to netSocket/netBind/netSendTo/...
# because PSL1GHT has two networking families that don't share descriptors:
# socket() returns 0x4000002D and netPoll answers POLLNVAL; netSocket()
# returns 46 and netPoll waits for whatever you ask it to. Measured 2026-09-03.
#
# Of the ten, GR33N was already using nine in link.c and net_tls.c. The tenth,
# netGetSockName, nobody had used -- and a networking function that doesn't
# exist shows up late and badly: with no declaration it's a warning, not an
# error, and linking in C doesn't check signatures. So all of them get asked
# about here, one at a time, and whichever fails comes out by name.
echo
echo ">> the net* family, which is the way socket.c goes now"

FALTAN_NET=""
for fn in netSocket netBind netConnect netClose netSend netRecv \
          netSendTo netRecvFrom netSetSockOpt netGetSockName netPoll; do
	case "$fn" in
	netSocket)      LLAMADA="return $fn(AF_INET, SOCK_DGRAM, IPPROTO_UDP);" ;;
	netBind|netConnect)
	                LLAMADA="return $fn(0, (struct sockaddr*)&a, sizeof(a));" ;;
	netClose)       LLAMADA="return $fn(0);" ;;
	netSend|netRecv)
	                LLAMADA="return $fn(0, b, sizeof(b), 0);" ;;
	netSendTo|netRecvFrom)
	                LLAMADA="return $fn(0, b, sizeof(b), 0, (struct sockaddr*)&a, (void*)&l);" ;;
	netSetSockOpt)  LLAMADA="return $fn(0, SOL_SOCKET, SO_REUSEADDR, b, sizeof(int));" ;;
	netGetSockName) LLAMADA="return $fn(0, (struct sockaddr*)&a, &l);" ;;
	netPoll)        LLAMADA="return $fn(&p, 1, 1);" ;;
	esac

	if ! enlaza "$fn" "#include <net/net.h>
#include <netinet/in.h>
#include <sys/socket.h>
int main(void){ struct sockaddr_in a; socklen_t l = sizeof(a);
char b[8]; struct pollfd p; (void)a; (void)b; (void)p; (void)l;
$LLAMADA }"; then
		FALTAN_NET="$FALTAN_NET $fn"
		sed 's/^/        /' "$PROBE/p.err" | head -4
	fi
done

if [ -n "$FALTAN_NET" ]; then
	echo
	echo "!! MISSING from the net* family:$FALTAN_NET"
	echo "   The socket.c in the patch uses them. If the error above is"
	echo "   about a signature and not a missing symbol, paste it to me and"
	echo "   I'll adjust the call; if it really isn't there, we work around it."
	exit 1
fi

# --- sources -----------------------------------------------------------

if [ ! -d "$SRC" ]; then
	echo
	echo ">> cloning libpeer"
	git clone --quiet https://github.com/sepfy/libpeer "$SRC"
else
	echo ">> using $SRC (already cloned)"
fi

cd "$SRC"

# To the pinned commit, and clean. Same as in usrsctp: nobody edits this by
# hand, so putting it back the way it came loses nothing, and without this a
# new patch never applies over the one from the previous round.
git checkout --quiet "$LIBPEER_REF" 2>/dev/null || {
	git fetch --quiet origin "$LIBPEER_REF" && git checkout --quiet "$LIBPEER_REF"
}
git checkout --quiet -- . 2>/dev/null || true
git clean -qfd 2>/dev/null || true

if git apply --check "$HERE/libpeer-ps3.patch" 2>/dev/null; then
	git apply "$HERE/libpeer-ps3.patch"
	echo ">> PS3 patch applied"
else
	echo "!! the patch doesn't apply. Paste this:"
	git apply --verbose "$HERE/libpeer-ps3.patch" 2>&1 | head -20
	exit 1
fi

cd - > /dev/null

# --- compilation --------------------------------------------------------

CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell"

# The shim FIRST: that's where <endian.h> comes from, and that's where libpeer
# gets __BYTE_ORDER to pick the layout of RtpHeader and RtcpHeader. If that
# resolved wrongly there would be no error: there would be misread RTP packets.
CFLAGS="$CFLAGS -I$SHIM"

CFLAGS="$CFLAGS -I$SRC/src"
CFLAGS="$CFLAGS -I$MBED/include -I$SRTP/include -I$SCTP/include"

# THE SAME mbedTLS CONFIGURATION mbedTLS ITSELF WAS COMPILED WITH.
#
# This was missing and it showed straight away: dtls_srtp.c couldn't find
# mbedtls_ssl_srtp_profile, MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
# mbedtls_ssl_conf_dtls_srtp_protection_profiles or
# mbedtls_ssl_get_dtls_srtp_negotiation_result -- ten errors -- right
# after build-mbedtls.sh had confirmed that DTLS-SRTP WAS there in the
# library.
#
# Both things were true at once, and that's the whole of it: the library had
# DTLS-SRTP because it was built with ps3_mbedtls_config.h, and libpeer didn't
# see it because it was compiled WITHOUT it, reading the mbedTLS headers in
# their default state, where MBEDTLS_SSL_DTLS_SRTP comes commented out.
#
# AND IT ISN'T ONLY A MATTER OF MISSING FUNCTIONS. The mbedTLS configuration
# also keeps STRUCT FIELDS behind #if. libpeer carries a whole
# mbedtls_ssl_context inside DtlsSrtp: compiling it against one configuration
# and linking it against another gives structs of different sizes on either
# side of the link, which is memory corruption without a single warning.
# Here the failure was loud, luckily; the silent version of this same mistake
# is the one that's frightening.
#
# RULE: everything that compiles against these headers carries this -D. The
# GR33N Makefile already does it (line 72); build-libsrtp.sh didn't and it
# has been given it too.
CFLAGS="$CFLAGS -DMBEDTLS_USER_CONFIG_FILE=\"ps3_mbedtls_config.h\""

CFLAGS="$CFLAGS $PSL_INC"

CFLAGS="$CFLAGS -DGR33N_PS3=1 $SELECT_DEF $ISSET_DEF"

# No signalling: out go coreHTTP, coreMQTT and cJSON.
#
# That's for the mode where libpeer connects to an MQTT broker and negotiates
# the SDP on its own. We already have the session set up -session.c talks to
# the xCloud v5 API- and we hand it the offer and the answer ourselves.
# peer_signaling.c and ssl_transport.c sit entirely inside an #ifndef, so they
# compile to nothing.
CFLAGS="$CFLAGS -DDISABLE_PEER_SIGNALING=1"

# The data channels go through usrsctp and not through libpeer's own SCTP.
# See claude/webrtc-portado.md: theirs doesn't retransmit, and xCloud input
# stops being applied the moment the server sees a gap in the numbering.
CFLAGS="$CFLAGS -DCONFIG_USE_USRSCTP=1"

# libpeer's logs go to peer_log(), which GR33N sends on to the debug server.
# Launching from the XMB there is no TTY: what doesn't reach the PC doesn't
# exist.
#
# AND LEVEL 2 (INFO), NOT DEBUG. On DEBUG libpeer writes one line PER RTP
# PACKET. At 60 fps that's hundreds a second down a UDP socket: it drowns the
# log and jams the thread. It's green-nx's warning, and they walked into it.
CFLAGS="$CFLAGS -DLOG_REDIRECT=1 -DLOG_LEVEL=2"

# For the same reason as in libSRTP and usrsctp: 32-bit words get read out of
# char buffers all over the place.
CFLAGS="$CFLAGS -fno-strict-aliasing"

# THE FLAG THAT HAS ALREADY CLAIMED TWO BUGS IN usrsctp.
#
# Without this, a macro or a function that doesn't exist turns into an
# implicit call, which in C99 is a warning. And when the missing thing
# returned a POINTER, in a 32-bit binary the int the compiler invents is
# exactly the same size: it compiles, it links, it starts, and it writes to a
# truncated address inside the console.
CFLAGS="$CFLAGS -Werror=implicit-function-declaration -Werror=implicit-int"

CFLAGS="$CFLAGS -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function"

# --- AND THAT THE CONFIGURATION REALLY ARRIVES -------------------------
#
# With the flags already assembled, it gets checked at the exact spot where
# it matters. libmbedtls.a having DTLS-SRTP and libpeer SEEING IT are two
# different things, and this round they came out different.
#
# Ten "unknown type name" errors that don't mention the configuration
# anywhere turn into one line here that says what is going on.
printf '#include <mbedtls/ssl.h>\n#if !defined(MBEDTLS_SSL_DTLS_SRTP)\n#error no llega\n#endif\nint main(void){return 0;}\n' > "$PROBE/c.c"
# shellcheck disable=SC2086
if $CC -c "$PROBE/c.c" -o "$PROBE/c.o" $CFLAGS > /dev/null 2>&1; then
	echo ">> the mbedTLS configuration reaches libpeer (DTLS-SRTP visible)"
else
	echo "!! THE mbedTLS HEADERS DO NOT BRING DTLS-SRTP."
	echo
	echo "   The library can have it and this still happen: if the"
	echo "   -DMBEDTLS_USER_CONFIG_FILE doesn't arrive, the headers get read"
	echo "   in their default state, where MBEDTLS_SSL_DTLS_SRTP comes"
	echo "   commented out."
	echo
	echo "   Check that $MBED/include/ps3_mbedtls_config.h is there"
	echo "   (build-mbedtls.sh copies it in when it installs)."
	exit 1
fi

FILES="
$SHIM/ps3_net_stubs.c
src/address.c
src/agent.c
src/base64.c
src/dtls_srtp.c
src/ice.c
src/mdns.c
src/peer.c
src/peer_connection.c
src/peer_signaling.c
src/ports.c
src/rtcp.c
src/rtp.c
src/sctp.c
src/sdp.c
src/socket.c
src/ssl_transport.c
src/stun.c
src/utils.c
"

OBJ="$WORK/.obj-libpeer"

# IT GETS BUILT OFF TO ONE SIDE AND SWAPPED IN AT THE END.
#
# This used to do rm -rf "$OUT" up here, and a failed build left GR33N with
# no peer.h: the next make died with "peer.h: No such file or directory",
# which has NOTHING to do with the real failure and sends you looking in the
# wrong place. It happened exactly like that on 2026-09-01 with utils.c.
#
# A dependency that fails cannot be allowed to take out the installation that
# was already working. It's built in a directory of its own and only put in
# place once everything has gone well, checks included.
FINAL="$OUT"
OUT="$WORK/.libpeer-ps3-en-obras"
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

echo
echo ">> compiling with $(basename "$CC")"

n=0
fail=0
for f in $FILES; do
	b=$(basename "$f" .c)

	case "$f" in
	/*) ruta="$f" ;;
	*)  ruta="$SRC/$f" ;;
	esac

	if [ ! -f "$ruta" ]; then
		echo "   DOES NOT EXIST $ruta"
		fail=$((fail + 1))
		continue
	fi

	# shellcheck disable=SC2086
	if $CC -c "$ruta" -o "$OBJ/$b.o" $CFLAGS 2> "$OBJ/$b.err"; then
		n=$((n + 1))
	else
		fail=$((fail + 1))
		echo "   FAILS $f"
		grep -E "error:" "$OBJ/$b.err" | sed 's/^/     /'
	fi
done

echo ">> $n compiled, $fail failed"

if [ "$fail" -ne 0 ]; then
	echo
	echo "   The full error files are in $OBJ/*.err"
	echo "   Paste this output and we carry on."
	exit 1
fi

"$AR" rcs "$OUT/lib/libpeer.a" "$OBJ"/*.o
cp "$SRC/src/peer.h" "$SRC/src/peer_connection.h" "$OUT/include/"
[ -f "$SRC/src/peer_signaling.h" ] && cp "$SRC/src/peer_signaling.h" "$OUT/include/"

# --- what is left unresolved --------------------------------------------

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libpeer.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libpeer.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	comm -23 "$WORK/.undef" "$WORK/.def" > "$WORK/.falta"

	echo
	echo ">> unresolved symbols:"
	sed 's/^/   /' "$WORK/.falta"

	echo
	echo "   Expected, and it all resolves when the EBOOT is linked:"
	echo
	echo "     newlib libc          memcpy, malloc, printf, usleep..."
	echo "     the console network  socket, bind, connect, select,"
	echo "                          sendto, inet_pton, netGetHostByName"
	echo "     mbedtls_*            libmbedtls.a"
	echo "     srtp_*               libsrtp2.a"
	echo "     usrsctp_*            libusrsctp.a"
	echo "     getifaddrs and co.   ps3_stubs.c, inside libusrsctp.a"
	echo "     mbedtls_timing_*     source/dtls_timer.c from GR33N"
	echo
	echo "   AND ONE that doesn't exist yet and has to be written:"
	echo
	echo "     peer_log   libpeer's logs to the remote log. With"
	echo "                LOG_REDIRECT=1, libpeer sends there everything"
	echo "                it would say on stdout, and from the XMB there"
	echo "                is no TTY: what doesn't reach the PC doesn't"
	echo "                exist. The signature is in src/utils.h:24."

	# getaddrinfo must NOT show up: if it does, the ports_resolve_addr
	# part of the patch hasn't gone in and the EBOOT won't link.
	if grep -qx "getaddrinfo" "$WORK/.falta"; then
		echo
		echo "!! getaddrinfo SHOWS UP, and PSL1GHT doesn't have it."
		echo "   The patch rewrites ports_resolve_addr() on top of"
		echo "   netGetHostByName; if the symbol is still there, that part"
		echo "   of the patch hasn't been applied."
		rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
		exit 1
	fi

	# select MUST NOT SHOW UP. And this check has changed meaning twice,
	# so it's worth setting down why.
	#
	# The first version complained as soon as it saw select unresolved.
	# That was wrong: if the console has it, select comes out unresolved
	# like socket or sendto and is fixed when the EBOOT is linked. The
	# second version only checked if the probe had said it was missing.
	#
	# The right answer now is a DIFFERENT one: select must NEVER come
	# out, resolved or unresolved, because on the PS3 it can't be called.
	# A descriptor here is worth 0x40000026 and FD_SET(0x40000026, ...)
	# indexes element 16,777,216 of an array of 16 -- a write 64 MB
	# past the end of the stack, and the console stops dead. Measured
	# 2026-09-01 with WEBRTC_DEBUG.
	#
	# The patch swaps the three sites (agent.c, mdns.c,
	# ssl_transport.c) for netPoll, which puts the descriptor in an int
	# and has no ceiling. So if select pokes out here, one of the three
	# hasn't been patched -- or libpeer has grown a fourth.
	if grep -qx "select" "$WORK/.falta" || grep -qx "select" "$WORK/.def"; then
		echo
		echo "!! select SHOWS UP, AND ON THE PS3 THAT HANGS THE CONSOLE."
		echo "   The known sites are agent.c, mdns.c and"
		echo "   ssl_transport.c, and the patch swaps them for netPoll."
		echo "   If it shows up anyway, find who calls it with:"
		echo "     ppu-nm -A $OUT/lib/libpeer.a | grep ' U select'"
		rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
		exit 1
	fi

	rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
fi

rm -rf "$OBJ"

# Everything has gone well: now the good installation does get swapped.
rm -rf "$FINAL"
mv "$OUT" "$FINAL"
OUT="$FINAL"

echo
echo ">> done: $OUT/lib/libpeer.a  ($(du -h "$OUT/lib/libpeer.a" | cut -f1))"
echo
echo "   The four libraries are there. Next is wiring them into the GR33N"
echo "   Makefile and writing the layer that uses them."
