#!/bin/sh
#
# GR33N - builds usrsctp for PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-usrsctp.sh
#
# Leaves $HOME/.gr33n-deps/usrsctp-ps3/ with:
#   include/usrsctp.h
#   lib/libusrsctp.a
#
# WHY usrsctp IS IN THE PROJECT AT ALL. xCloud's input runs over four SCTP
# data channels -control, input, message, chat-, all ORDERED and RELIABLE,
# and the input protocol v1 sends the controller's absolute state numbered
# by a sequence: the server stops applying it the moment it sees a gap.
# libpeer brings its own 733-line SCTP that does NOT retransmit (faced with
# a gap it does `tsn = cumulative_tsn_ack + 1`, with the real logic commented
# out right next to it), so it will not do. See claude/webrtc-portado.md.
#
# THE FIRST THING THIS SCRIPT DOES IS NOT COMPILE: it is finding out whether
# PSL1GHT brings pthreads, because that decides whether this is a 114-line
# patch or five hundred.
#
# usrsctp wants pthread_mutex_t, pthread_rwlock_t, pthread_cond_t and
# pthread_t (sctp_os_userspace.h:299-304). If the console brings them, it
# just compiles. If not, someone has to write a layer that translates them
# into lv2 threads -sysMutexCreate, sysCondCreate, sysThreadCreate-, and
# that is a whole other job. The probe below says which in two seconds
# instead of it being discovered among twenty-three files' worth of errors.
#
# EVERYTHING ELSE IS ALREADY PROVEN, though not with ppu-gcc: the 23 files
# compile clean for PowerPC64 big-endian with the Debian cross-compiler.
# The only errors that came up there were over sa_len -- the PC's glibc
# does not have it and PSL1GHT's newlib does-, i.e. exactly the opposite
# of the problem we would have here.

set -e

: "${PS3DEV:=/usr/local/ps3dev}"

CC="$PS3DEV/ppu/bin/ppu-gcc"
AR="$PS3DEV/ppu/bin/ppu-ar"
NM="$PS3DEV/ppu/bin/ppu-nm"

HERE=$(cd "$(dirname "$0")" && pwd)

WORK="${GR33N_DEPS:-$HOME/.gr33n-deps}"
SRC="$WORK/usrsctp-src"
OUT="$WORK/usrsctp-ps3"

mkdir -p "$WORK"

if [ ! -x "$CC" ]; then
	echo "cannot find $CC"
	exit 1
fi

# --- the PSL1GHT headers -----------------------------------------------
#
# They are not on ppu-gcc's default path: $(LIBPSL1GHT_INC) puts them there
# from ppu_rules, i.e. the project's Makefile. Compiling by hand, nobody
# sets that, and that already cost us a round with libSRTP.

PSL_INC=""
for d in "$PS3DEV/ppu/include" "$PSL1GHT/ppu/include" \
         "$PS3DEV/portlibs/ppu/include"; do
	if [ -f "$d/netinet/in.h" ]; then
		PSL_INC="-I$d"
		PSL_DIR="$d"
		echo ">> PSL1GHT headers at $d"
		break
	fi
done

if [ -z "$PSL_INC" ]; then
	echo "cannot find netinet/in.h in any PSL1GHT path."
	echo "Find out where it is with:  find \$PS3DEV -name in.h -path '*netinet*'"
	exit 1
fi

# --- THE PROBE ---------------------------------------------------------

echo
echo ">> looking at what the console brings"

PROBE="$WORK/.probe-usrsctp"
mkdir -p "$PROBE"

# -Werror=implicit-function-declaration IS NOT DECORATION: WITHOUT IT THE
# PROBE LIES.
#
# Last round this probe said the console brought CMSG_DATA, CMSG_SPACE
# and CMSG_LEN. It does not. The test program was:
#
#     struct cmsghdr c; (void)CMSG_DATA(&c);
#     return (int)(CMSG_SPACE(4)+CMSG_LEN(4));
#
# ...and that COMPILES even though the macros do not exist, because in C99
# calling something undeclared is a warning: the compiler makes up
# `int CMSG_DATA()` and carries on. The probe checked the exit code, saw
# zero, and said YES.
#
# This is the third time this week a probe has measured the wrong thing:
# before it was BYTE_ORDER asking about a header usrsctp never includes,
# and sys/time.h asking the filesystem instead of the compiler. This is
# the funniest one, because the probe exists precisely so as not to
# assume. With these two flags the warning becomes the error it should
# always have been, and the answer can be trusted.
PEDANTE="-Werror=implicit-function-declaration -Werror=implicit-int"

# THE pthreads ONES HAVE TO LINK, NOT JUST COMPILE.
#
# This probe said yes to all five pthreads questions, and it was lying.
# It compiled with -c, which means what it was actually answering was "the
# headers declare pthread_mutex_lock" -- true, and not the question. The
# question is whether the SYMBOL exists, and only the linker knows that.
#
# The failure showed up right at the very end, linking the EBOOT, as a
# thousand lines of "undefined reference to pthread_mutex_lock" from
# libusrsctp.a. With the whole job already built on top of it.
#
# It is the same CMSG lesson -- "compiles" does not mean "exists" --
# applied to the most expensive question in the whole port, which is
# exactly the one this script claimed to be answering.
# -lpthread goes BEFORE -lnet, just as in the Makefile's LIBS: if the
# probe does not link with the same line as the EBOOT, it is not
# measuring the same thing, and a probe that does not measure the same
# thing lies.
LIBS_SONDA="-lpthread -lnet -lnetctl -lsysmodule -lrt -llv2 -lm"
PSL_LIB=""
[ -n "$PSL_DIR" ] && PSL_LIB="-L$(dirname "$PSL_DIR")/lib"

probar() {
	nombre=$1
	codigo=$2
	solo_compilar=$3      # not empty = compiling is enough

	printf '%s\n' "$codigo" > "$PROBE/p.c"

	if [ -n "$solo_compilar" ]; then
		# shellcheck disable=SC2086
		if $CC -c "$PROBE/p.c" -o "$PROBE/p.o" -std=gnu99 $PEDANTE \
		   $PSL_INC 2> "$PROBE/p.err"; then
			echo "   YES  $nombre"
			return 0
		fi
	else
		# shellcheck disable=SC2086
		if $CC "$PROBE/p.c" -o "$PROBE/p.elf" -std=gnu99 $PEDANTE \
		   $PSL_INC $PSL_LIB $LIBS_SONDA 2> "$PROBE/p.err"; then
			echo "   YES  $nombre"
			return 0
		fi
	fi

	echo "   NO   $nombre"
	return 1
}

falta=0

probar "pthread.h" '#include <pthread.h>
int main(void){return 0;}' || falta=1

probar "pthread_mutex_t + lock/unlock" '#include <pthread.h>
pthread_mutex_t m;
int main(void){pthread_mutex_init(&m,0);pthread_mutex_lock(&m);
pthread_mutex_unlock(&m);return 0;}' || falta=1

probar "pthread_rwlock_t" '#include <pthread.h>
pthread_rwlock_t r;
int main(void){pthread_rwlock_init(&r,0);pthread_rwlock_rdlock(&r);
pthread_rwlock_unlock(&r);return 0;}' || falta=1

probar "pthread_cond_t" '#include <pthread.h>
pthread_cond_t c; pthread_mutex_t m;
int main(void){pthread_cond_init(&c,0);pthread_cond_wait(&c,&m);
pthread_cond_signal(&c);return 0;}' || falta=1

probar "pthread_create + join" '#include <pthread.h>
static void *f(void *a){(void)a;return 0;}
int main(void){pthread_t t;pthread_create(&t,0,f,0);pthread_join(t,0);
return 0;}' || falta=1

probar "sys/socket.h" '#include <sys/socket.h>
int main(void){return 0;}' 1 || falta=1

probar "sockaddr with sa_len (BSD family)" '#include <netinet/in.h>
int main(void){struct sockaddr_in a; a.sin_len=0; return a.sin_len;}' 1 || falta=1

# THIS ONE DOES NOT COUNT TOWARDS `falta`: it is not a requirement, it is
# a question.
#
# PSL1GHT defines struct iovec -- something that includes its sys/socket.h
# pulls it in -- but it does NOT have the file sys/uio.h. That combination
# is unusual and has to be asked about, because from the preprocessor there
# is no way to know whether a struct already exists, and PSL1GHT does not
# flag it with any of the usual macros.
IOVEC_DEF=""
if probar "struct iovec (console already brings it)" '#include <sys/socket.h>
int main(void){struct iovec v; v.iov_base=0; v.iov_len=0;
return (int)v.iov_len;}' 1; then
	IOVEC_DEF="-DGR33N_HAVE_IOVEC=1"
fi

# And the same with IPv6. It does not count towards `falta` either: the PS3
# does not speak IPv6 and that is fine. But usrsctp declares fields of type
# struct in6_addr and struct sockaddr_in6 WITHOUT guarding them behind INET6
# (user_inpcb.h:72 and :77, usrsctp.h:152), inside unions that get reserved
# whole. The type has to exist even if it is never used; compiling with
# plain -DINET does not skip those lines, only the code that looks at them.
IN6_DEF=""
if probar "struct in6_addr (console already brings it)" '#include <netinet/in.h>
int main(void){struct in6_addr a; struct sockaddr_in6 s;
(void)a;(void)s;return 0;}' 1; then
	IN6_DEF="-DGR33N_HAVE_IN6=1"
fi

# And sockaddr_storage, which usrsctp declares in sctp_uio.h:348 and :574
# inside structures that get reserved whole.
SS_DEF=""
if probar "struct sockaddr_storage (already brings it)" '#include <sys/socket.h>
int main(void){struct sockaddr_storage a;(void)a;return 0;}' 1; then
	SS_DEF="-DGR33N_HAVE_SS=1"
fi

# And the last one: struct in_pktinfo, only needed if the console defines
# IP_PKTINFO. If it defines neither that nor IP_RECVDSTADDR, user_recv_thread.c
# stops dead with a #error; the netinet/in.h wrapper declares the BSD one
# for that case.
PKT_DEF=""
if probar "struct in_pktinfo (already brings it)" '#include <netinet/in.h>
int main(void){struct in_pktinfo a;(void)a;return 0;}' 1; then
	PKT_DEF="-DGR33N_HAVE_PKTINFO=1"
fi

echo

if [ "$falta" -ne 0 ]; then
	cat <<'FIN'
>> SOMETHING IS MISSING, and that changes the plan.

   usrsctp uses pthreads directly in sctp_os_userspace.h (lines
   299-304): mutex, rwlock, condition variable and threads. If PSL1GHT
   does not bring them, someone has to write a translation layer to lv2:

     pthread_mutex_t   -> sys_mutex_t      (sysMutexCreate/Lock/Unlock)
     pthread_cond_t    -> sys_cond_t       (sysCondCreate/Wait/Signal)
     pthread_rwlock_t  -> a plain mutex    (SCTP does not depend on two
                          readers going in at once; losing that costs
                          performance, not correctness)
     pthread_t         -> sys_ppu_thread_t (sysThreadCreate/Join)

   It is a shim header and a handful of wrappers, not a
   redesign. But it is work, and better to know now.

   Paste this whole output and we carry on from there.
FIN
	exit 1
fi

# --- THE BSD TYPES, GENERATED AND NOT GUESSED --------------------------
#
# The headers that come from FreeBSD use the good old BSD names:
# u_int16_t, u_char, caddr_t. Newlib brings some and not others, and which
# ones exactly depends on how it was built -- it is not a list you can
# know from memory.
#
# So they get tested one by one and a header gets written with ONLY the
# ones that are missing. Defining all of them outright would clash with
# the ones that do exist (redefining a typedef is an error in C99), and
# defining too few leaves the same failure we already had.
#
# This came from in_systm.h in FreeBSD using u_int16_t, which ppu-gcc does
# not know: 22 files with the same error. Third time a borrowed header
# has brought in a dependency the console does not have, so this time it
# is the CLASS of problem being fixed, not the instance.

TIPOS="$WORK/.bsdtypes"
mkdir -p "$TIPOS"
BSDH="$TIPOS/gr33n_bsdtypes.h"

{
	echo "/* GENERATED by build-usrsctp.sh. Do not edit: rebuilt every time."
	echo " * Contains ONLY the BSD types this newlib does not bring. */"
	echo "#ifndef GR33N_BSDTYPES_H"
	echo "#define GR33N_BSDTYPES_H"
	echo "#include <stdint.h>"
	echo "#include <sys/types.h>"
} > "$BSDH"

echo ">> BSD types that need adding"
n_tipos=0

tipo_bsd() {
	printf '#include <sys/types.h>\n#include <stdint.h>\n%s x;\nint main(void){(void)x;return 0;}\n' \
		"$1" > "$PROBE/t.c"

	if $CC -c "$PROBE/t.c" -o "$PROBE/t.o" -std=gnu99 $PEDANTE $PSL_INC > /dev/null 2>&1; then
		return 0
	fi

	echo "typedef $2 $1;" >> "$BSDH"
	echo "   $1 is missing, defined as $2"
	n_tipos=$((n_tipos + 1))
}

tipo_bsd u_int8_t   uint8_t
tipo_bsd u_int16_t  uint16_t
tipo_bsd u_int32_t  uint32_t
tipo_bsd u_int64_t  uint64_t
tipo_bsd u_char     "unsigned char"
tipo_bsd u_short    "unsigned short"
tipo_bsd u_int      "unsigned int"
tipo_bsd u_long     "unsigned long"
tipo_bsd caddr_t    "char *"

# n_short, n_long and n_time do NOT go here: netinet/in_systm.h defines
# them, which is where they always belong, and putting them in both
# places is a duplicate typedef, which in C99 is an error, not a warning.

# --- AND BYTE_ORDER, THE WORST OF THE LOT ------------------------------
#
# FreeBSD's netinet/ip.h declares struct ip TWICE, once for each byte
# order:
#
#     #if BYTE_ORDER == LITTLE_ENDIAN
#             u_char ip_hl:4, ip_v:4;
#     #endif
#     #if BYTE_ORDER == BIG_ENDIAN
#             u_char ip_v:4, ip_hl:4;
#     #endif
#
# If BYTE_ORDER is not defined, the C preprocessor treats unknown
# identifiers as ZERO. Which means both comparisons come out 0 == 0,
# true, and BOTH BRANCHES get compiled: "duplicate member
# ip_v", 22 times.
#
# And take care over what this really means. Here the failure was noisy
# because the members repeat. But the same mechanism, in a header where
# the two branches did not clash, would pick the little-endian one IN
# SILENCE on a big-endian machine. That is exactly the failure we have
# been chasing all week, and here it is produced by a missing macro.
#
# It asks whether the console has it set up correctly; if not, it is set
# here, in the header that goes in with -include before any other.

# THE FIRST VERSION OF THIS PROBE ASKED THE WRONG QUESTION.
#
# It included <machine/endian.h> and checked that BYTE_ORDER came out of
# it set correctly. It did, and it said "the console has it set up
# correctly" -- and then the same 22 files failed anyway, because usrsctp
# NEVER includes that header at all. The right question was not "can
# BYTE_ORDER be obtained" but "is BYTE_ORDER set by the time ip.h is read".
#
# One header after another gets tried and THE CONSOLE'S OWN is used, with
# its values, instead of making up ones of our own that might not match.

END_INC=""
for cab in machine/endian.h sys/endian.h endian.h; do
	{
		printf '#include <%s>\n' "$cab"
		printf '#if !defined(BYTE_ORDER) || !defined(BIG_ENDIAN)\n'
		printf '#error faltan\n#endif\n'
		printf '#if BYTE_ORDER != BIG_ENDIAN\n#error no es big-endian\n#endif\n'
		printf 'int main(void){return 0;}\n'
	} > "$PROBE/e.c"

	if $CC -c "$PROBE/e.c" -o "$PROBE/e.o" -std=gnu99 $PSL_INC > /dev/null 2>&1; then
		END_INC="$cab"
		break
	fi
done

if [ -n "$END_INC" ]; then
	echo "   BYTE_ORDER: comes from <$END_INC>, and is forced in every file"
	{
		echo ""
		echo "/* From the console itself, with ITS values. Without this,"
		echo " * netinet/ip.h compiles both its byte-order branches at the"
		echo " * same time: in C an unknown identifier inside an #if"
		echo " * is worth 0, and 0 == 0 is true both times. */"
		echo "#include <$END_INC>"
	} >> "$BSDH"
else
	echo "   BYTE_ORDER: no header provides it; setting it by hand"
	{
		echo ""
		echo "/* No header from the console provides it, so it goes in by hand."
		echo " * Without this, netinet/ip.h compiles both its branches at once. */"
		echo "#ifndef LITTLE_ENDIAN"
		echo "#define LITTLE_ENDIAN 1234"
		echo "#endif"
		echo "#ifndef BIG_ENDIAN"
		echo "#define BIG_ENDIAN 4321"
		echo "#endif"
		echo "#ifndef BYTE_ORDER"
		echo "#define BYTE_ORDER BIG_ENDIAN"
		echo "#endif"
	} >> "$BSDH"
fi

n_tipos=$((n_tipos + 1))

echo "#endif" >> "$BSDH"

if [ "$n_tipos" -eq 0 ]; then
	echo "   none: the console brings them all"
fi

# --- WHERE sysGetRandomNumber COMES FROM -------------------------------
#
# THIS BLOCK EXISTS BECAUSE I MADE IT UP.
#
# The user_environment.c patch put `#include <lv2/random.h>` because it
# sounded like what it should be called. It does not exist. The 23 files
# did not notice -only that one did-, but it is exactly the same class
# of failure we have had all week: the plausible answer, used unchecked.
#
# What is actually known: GR33N's source/net_tls.c calls
# sysGetRandomNumber and has been compiling for weeks. But it includes
# SEVEN PSL1GHT headers and there is no way to know from here which one
# declares it -the headers are on your WSL, not on mine-. So they are
# tried one by one, with -Werror=implicit-function-declaration so that
# "compiles with a warning" does not count as a success.
#
# If none of them work, the prototype is left by hand and a warning is
# printed to the screen: it has to be checked against the real header
# before trusting it, because a wrong signature here gives no error, it
# gives broken entropy. And a broken entropy source does not show itself:
# it gives predictable keys.

RNDH="$TIPOS/gr33n_ps3_random.h"
RND_CAB=""

echo
echo ">> where sysGetRandomNumber comes from"

for cab in lv2/system.h sys/random_number.h lv2/random_number.h ppu-lv2.h \
           lv2/lv2.h sys/systime.h lv2/systime.h net/net.h; do
	{
		printf '#include <%s>\n' "$cab"
		printf 'int main(void){unsigned char b[8];\n'
		printf 'sysGetRandomNumber(b, 8); return 0;}\n'
	} > "$PROBE/r.c"

	if $CC -c "$PROBE/r.c" -o "$PROBE/r.o" -std=gnu99 $PEDANTE $PSL_INC \
	   > /dev/null 2>&1; then
		RND_CAB="$cab"
		break
	fi
done

{
	echo "/* GENERATED by build-usrsctp.sh. Do not edit: rebuilt every time."
	echo " * Included by the user_environment.c patch. */"
	echo "#ifndef GR33N_PS3_RANDOM_H"
	echo "#define GR33N_PS3_RANDOM_H"
} > "$RNDH"

if [ -n "$RND_CAB" ]; then
	echo "   <$RND_CAB> declares it"
	echo "#include <$RND_CAB>" >> "$RNDH"
else
	echo "   !! NONE of the candidates declare it."
	echo "      Setting the prototype by hand, BUT IT HAS TO BE CHECKED:"
	echo "      find the right one with"
	echo "        grep -rl sysGetRandomNumber \$PS3DEV/ppu/include"
	echo "      and paste me the declaration line."
	{
		echo "/* THE HEADER WAS NOT FOUND. Prototype deduced from the"
		echo " * call in source/net_tls.c:245, which compiles:"
		echo " *     ret = sysGetRandomNumber(tmp, (u64)ask);"
		echo " * If the real signature is not this one, the link goes"
		echo " * through just the same -in C there is no name mangling-"
		echo " * and what comes out broken is the entropy, silently. */"
		echo "int sysGetRandomNumber(void *addr, unsigned long long size);"
	} >> "$RNDH"
fi

echo "#endif" >> "$RNDH"

echo
echo ">> the console brings everything usrsctp asks for. Onward."
echo

# --- the headers PSL1GHT is missing -------------------------------------
#
# deps/sonda-cabeceras.sh asked about 43 and PSL1GHT brings 27. 16 are
# missing, and on top of that its sys/queue.h exists with NOT A SINGLE
# TAILQ macro, which usrsctp uses everywhere -- 68 different macros of
# LIST, SLIST, STAILQ and TAILQ.
#
# The wire-format ones (netinet/ip.h, udp.h) and sys/queue.h come from
# FreeBSD as-is, under their licence. Writing a wire struct with bitfields
# from memory, on the platform where byte order has already bitten us
# this week, would be tempting fate.
#
# MIND THE TWIST: the TYPE struct iovec does exist in PSL1GHT -- something
# that includes its sys/socket.h defines it -- but the FILE does not. So
# the shim cannot define the struct blindly (that gives "redefinition")
# nor can it be left empty (UIO_MAXIOV is missing, which usrsctp uses in
# user_socket.c:585). The probe above resolves it, by asking instead of
# assuming. See the long comment in ps3-shim/sys/uio.h.
#
# No need to worry about readv and writev: in the whole of usrsctplib
# there is not a single call to either one.
#
# The directory goes AHEAD of the PSL1GHT headers on the path, but it
# only contains uio.h: the console keeps resolving everything else.

SHIM="$HERE/ps3-shim"

for f in sys/uio.h sys/queue.h sys/socket.h sys/time.h net/if.h ifaddrs.h \
         netinet/ip.h netinet/udp.h netinet/in_systm.h netinet/in.h \
         endian.h errno.h ps3_stubs.c; do
	if [ ! -f "$SHIM/$f" ]; then
		echo "missing $SHIM/$f"
		echo "the shim tree has to be at deps/ps3-shim/"
		exit 1
	fi
done

echo ">> shim headers at $SHIM"
echo "   (16 that PSL1GHT does not bring, plus sys/queue.h because its own"
echo "    does not have a single TAILQ macro. See $SHIM/README.md)"

# --- sources ------------------------------------------------------------

if [ ! -d "$SRC" ]; then
	echo ">> cloning usrsctp"
	git clone --depth 1 https://github.com/sctplab/usrsctp.git "$SRC"
else
	echo ">> using $SRC (already cloned)"
fi

# --- the patch ----------------------------------------------------------
#
# Three changes, and the first is the one that saves a wasted afternoon:
#
#   1. sockaddr_conn with the length byte in front. PSL1GHT's newlib
#      comes from BSD, so struct sockaddr carries sa_len and the family
#      goes at offset 1. Without this, sconn_family is read wrong and
#      the heap gets corrupted when creating the socket. Diagnosed from
#      green-nx, which ran into this on Switch with the same newlib family.
#   2. read_random on top of sysGetRandomNumber, with a check that it
#      does not return all zeros.
#   3. The same definition in the public header and in the internal one.

cd "$SRC"

# THE PREVIOUS ROUND'S PATCH IS UNDONE FIRST.
#
# $SRC is a clone of upstream and NOBODY edits it by hand: the only thing
# that changes in there is this patch. Putting it back as it came loses
# nothing.
#
# Without this, every time the patch changes the script gets stuck: the
# old one is already applied, the new one does not fit on top of it, and
# `git apply --reverse` does not either, because they are not the same
# patch. It would print "the patch does not apply to this version of
# usrsctp / upstream has probably changed", which is a false diagnosis
# that sends you looking in the wrong place.
if git rev-parse --git-dir > /dev/null 2>&1; then
	git checkout -- . 2>/dev/null || true
	git clean -fd > /dev/null 2>&1 || true
fi

if git apply --check "$HERE/usrsctp-ps3.patch" 2>/dev/null; then
	git apply "$HERE/usrsctp-ps3.patch"
	echo ">> PS3 patch applied"
elif git apply --reverse --check "$HERE/usrsctp-ps3.patch" 2>/dev/null; then
	echo ">> the PS3 patch was already applied"
else
	echo "!! the patch does not apply to this version of usrsctp."
	echo "   Upstream has probably changed. Paste this and I will redo it:"
	git apply --verbose "$HERE/usrsctp-ps3.patch" 2>&1 | head -20
	exit 1
fi
cd - > /dev/null

# --- compilation --------------------------------------------------------

CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell"

# The shim FIRST, so <sys/uio.h> is found there; the rest of the
# <sys/...> headers PSL1GHT still supplies, since there is nothing else in there.
CFLAGS="$CFLAGS -I$SHIM $IOVEC_DEF $IN6_DEF $SS_DEF $PKT_DEF"

# The missing BSD types, ahead of everything. See the block above.
CFLAGS="$CFLAGS -include $BSDH"

# And the directory of generated stuff, so that the user_environment.c
# patch finds gr33n_ps3_random.h.
CFLAGS="$CFLAGS -I$TIPOS"

CFLAGS="$CFLAGS -I$SRC/usrsctplib -I$SRC/usrsctplib/netinet $PSL_INC"

# THE FLAG THAT WOULD HAVE SAVED THE PREVIOUS ROUND.
#
# Without this, a macro that does not exist -CMSG_SPACE, timercmp,
# timingsafe_bcmp- turns into an implicit function call, which in C99 is
# a WARNING. The file compiles, goes into the library, and the failure
# shows up later, somewhere else:
#
#   - if the symbol does not exist, at link time, without saying who called it;
#   - if the macro returned a POINTER -CMSG_DATA, CMSG_NXTHDR-, it never
#     shows up: C99 assumes it returns int, and in a 32-bit binary that
#     int is the same size as the pointer. It compiles, links, starts,
#     and writes to a truncated address inside the console.
#
# That last case is the scary one, and it is exactly the one we had:
# sctp_indata.c does memcpy(CMSG_DATA(cmh), ...) on the receive path.
#
# WATCH WHAT THIS CAN DO TO THE COUNT. Files that "compiled" last round
# can fail now, because before they passed with warnings. That is NOT
# a regression: it is the same thing that was already wrong, said in time.
CFLAGS="$CFLAGS -Werror=implicit-function-declaration -Werror=implicit-int"

# PSL1GHT's sys/socket.h declares sysNetSelect with a struct timeval*
# before struct timeval exists, and that throws a warning in EVERY file.
# It is not our bug and it breaks nothing, but twenty-two copies of the
# same warning bury the ones that do matter. Including sys/time.h first,
# the type is already complete by the time that line is read.
#
# THE COMPILER IS ASKED, NOT THE FILESYSTEM. The previous version
# checked whether $PSL_DIR/sys/time.h existed and never found it,
# because that header lives in newlib's sysroot and not in PSL1GHT's
# directory. The warning kept coming out twenty-two times with nobody
# knowing why. Same mistake as with BYTE_ORDER: asking in the wrong place.
printf '#include <sys/time.h>\nint main(void){struct timeval t;(void)t;return 0;}\n' \
	> "$PROBE/tv.c"
if $CC -c "$PROBE/tv.c" -o "$PROBE/tv.o" -std=gnu99 $PSL_INC > /dev/null 2>&1; then
	CFLAGS="$CFLAGS -include sys/time.h"
fi

# THE LINE EVERYTHING WORKING DEPENDS ON.
#
# usrsctp declares its wire headers in duplicate and picks with
# WORDS_BIGENDIAN, which does NOT auto-detect for this platform: it only
# deduces itself for __APPLE__ on PowerPC (sctp_os_userspace.h:278).
# Without defining it, it compiles without a single warning with the
# little-endian structures on a big-endian machine, and every SCTP packet
# gets read wrong. There is no error: there is input that never arrives.
CFLAGS="$CFLAGS -DWORDS_BIGENDIAN=1"

# And who we are, for the patch's three branches.
CFLAGS="$CFLAGS -DGR33N_PS3=1"

# What usrsctp expects from a userspace host.
CFLAGS="$CFLAGS -D__Userspace__ -DSCTP_SIMPLE_ALLOCATOR -DSCTP_PROCESS_LEVEL_LOCKS"
CFLAGS="$CFLAGS -DHAVE_SA_LEN -DHAVE_SIN_LEN -DHAVE_SIN6_LEN -DHAVE_SCONN_LEN"

# IPv4 ONLY, AND ON PURPOSE.
#
# With -DINET6, user_recv_thread.c asks for struct in6_pktinfo, which
# belongs to raw IPv6 sockets and does not exist here. It could be
# fought, but there is no point: the PS3 does not speak IPv6, and the
# IPv6 address xCloud gives in /configuration is Teredo -- a tunnel
# over IPv4 that we could not use either way. Declaring support for
# something that cannot be done is the same kind of lie as the
# RESOLUTION=63 in PARAM.SFO that used to freeze the console.
#
# Checked with the cross-compiler: with INET6, 1 of 23 fails; without it, 23 of 23 do.
CFLAGS="$CFLAGS -DINET"

# Strict aliasing off, for the same reason as in libSRTP: SCTP reads
# 32-bit words from char buffers everywhere.
CFLAGS="$CFLAGS -fno-strict-aliasing"

# usrsctp comes from FreeBSD and drags along warnings that are not ours.
# The noisy ones get silenced, NEVER the ones about sizes or pointers.
CFLAGS="$CFLAGS -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function"

# --- THE SECOND PROBE, THIS TIME WITH THE REAL OPTIONS -----------------
#
# The probe above compiles with plain $PSL_INC. The build compiles with
# the shim tree IN FRONT, and there <sys/socket.h> is no longer the
# console's own but the ps3-shim wrapper. So the two questions are not
# the same question, and the one above cannot answer for this one.
#
# It is the same BYTE_ORDER lesson: measure where the problem happens.
# Here it is measured at the exact spot, and it also says WHO puts each
# thing, so nobody has to work it out from the errors next time.

quien_pone() {
	{
		printf '#include <%s>\n' "$2"
		printf '#ifdef %s\n#error lo pone el shim\n#endif\n' "$3"
		printf 'int main(void){return 0;}\n'
	} > "$PROBE/w.c"

	# shellcheck disable=SC2086
	if $CC -c "$PROBE/w.c" -o "$PROBE/w.o" $CFLAGS > /dev/null 2>&1; then
		echo "   $1: the console brings it"
	else
		echo "   $1: ps3-shim puts it in"
	fi
}

echo
echo ">> with the build's real options, who puts what"
quien_pone "CMSG_DATA/SPACE/LEN" sys/socket.h GR33N_CMSG_PUESTAS_AQUI
quien_pone "CMSG_ALIGN         " sys/socket.h GR33N_CMSG_ALIGN_PUESTA_AQUI
quien_pone "timercmp/add/sub   " sys/time.h   GR33N_TIMERCMP_PUESTA_AQUI
echo

# ps3_stubs.c goes into the library: the four interface-enumeration
# functions usrsctp calls that the console does not have. They return
# "nothing here", which with AF_CONN is the truth and not a fudge. See README.md.
FILES="
$SHIM/ps3_stubs.c
usrsctplib/netinet/sctp_asconf.c
usrsctplib/netinet/sctp_auth.c
usrsctplib/netinet/sctp_bsd_addr.c
usrsctplib/netinet/sctp_callout.c
usrsctplib/netinet/sctp_cc_functions.c
usrsctplib/netinet/sctp_crc32.c
usrsctplib/netinet/sctp_indata.c
usrsctplib/netinet/sctp_input.c
usrsctplib/netinet/sctp_output.c
usrsctplib/netinet/sctp_pcb.c
usrsctplib/netinet/sctp_peeloff.c
usrsctplib/netinet/sctp_sha1.c
usrsctplib/netinet/sctp_ss_functions.c
usrsctplib/netinet/sctp_sysctl.c
usrsctplib/netinet/sctp_timer.c
usrsctplib/netinet/sctp_userspace.c
usrsctplib/netinet/sctp_usrreq.c
usrsctplib/netinet/sctputil.c
usrsctplib/netinet6/sctp6_usrreq.c
usrsctplib/user_environment.c
usrsctplib/user_mbuf.c
usrsctplib/user_socket.c
"

# user_recv_thread.c IS NOT ON THE LIST, AND THAT IS ON PURPOSE.
#
# It is the mode where usrsctp opens its own sockets -one raw SCTP one
# and one UDP for the tunnel- with dedicated threads reading from them.
# 1500 lines that call socket(), bind(), setsockopt(), recvmsg() and
# close(), and PSL1GHT has none with those names: its API is
# netSocket/netBind/netRecv. They were five unresolved symbols at EBOOT
# link time, from code that never runs, because libpeer starts usrsctp with AF_CONN.
#
# The only TWO functions that file exports -recv_thread_init and
# recv_thread_destroy, checked with nm- are empty in ps3_stubs.c, which
# is the same thing WebRTC does with usrsctp_init_nothreads(). The long
# comment is there.

OBJ="$WORK/.obj-usrsctp"
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

echo ">> compiling with $(basename "$CC")"

n=0
fail=0
for f in $FILES; do
	b=$(basename "$f" .c)

	# ps3_stubs.c comes with an absolute path; the rest hangs off $SRC.
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
		# WHOLE, AND ONLY THE ERRORS.
		#
		# Before this was `head -10`, and in sctp_pcb.c it ate the third
		# error -an assignment to a pthread_t which is a struct- because
		# the first two took up the ten lines with their cursors. A whole
		# round lost to save on output.
		#
		# The warnings and the context lines are stripped and what is left
		# is the message of each error, which is the only thing worth reading.
		grep -E "error:|Error [0-9]" "$OBJ/$b.err" | sed 's/^/     /'
	fi
done

echo ">> $n compiled, $fail failed"

if [ "$fail" -ne 0 ]; then
	echo
	echo "   The complete error files -with warnings and context- are in"
	echo "   $OBJ/*.err  in case one of them needs looking at in full."
	echo "   Paste this output and we carry on."
	echo
	exit 1
fi

"$AR" rcs "$OUT/lib/libusrsctp.a" "$OBJ"/*.o
cp "$SRC/usrsctplib/usrsctp.h" "$OUT/include/"

# --- what is left unresolved --------------------------------------------

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libusrsctp.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libusrsctp.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	comm -23 "$WORK/.undef" "$WORK/.def" > "$WORK/.falta"

	echo
	echo ">> unresolved symbols:"
	sed 's/^/   /' "$WORK/.falta"

	echo
	echo "   Expected: newlib's libc (memcpy, malloc, printf, __errno...),"
	echo "   pthread_*, usleep, gettimeofday, and lv2's sysGetRandomNumber."

	# THIS IS NOT EYEBALLED: IT IS CHECKED.
	#
	# usrsctp speaks over AF_CONN, meaning it does NOT open sockets: we hand
	# it packets with usrsctp_conninput() and it gives them back to us
	# through the callback. The one who puts them on the wire is libpeer,
	# over DTLS, on the UDP socket GR33N already manages.
	#
	# So none of these names should be left pending. And besides, PSL1GHT
	# does not have them: its API is netSocket/netBind/netSetSockOpt/
	# netRecv/netSendTo/netClose. If they turn up, the EBOOT does not link,
	# and the error would show up much later and talking about something else.
	#
	# Last round this was a paragraph asking the reader to look. They
	# looked, and all six were there. A warning that has to be read is not
	# a check.
	RED=$(grep -x -E 'socket|bind|listen|accept|connect|sendmsg|recvmsg|sendto|recvfrom|setsockopt|getsockopt|ioctl|select|poll' \
	      "$WORK/.falta" || true)

	if [ -n "$RED" ]; then
		echo
		echo "!! THERE ARE UNRESOLVED NETWORK SYMBOLS:"
		printf '%s\n' "$RED" | sed 's/^/     /'
		echo
		echo "   With AF_CONN usrsctp should never open a socket, and"
		echo "   PSL1GHT does not have these names. As things stand, the EBOOT"
		echo "   is not going to link."
		echo
		echo "   To find out WHO is asking for them:"
		echo "     $NM --undefined-only $OUT/lib/libusrsctp.a | less"
		echo "   (the .o name shows up on the line above each block)"
		echo
		echo "   Paste that and we will take a look."
	else
		echo
		echo "   And none of them are network ones, which is what is supposed"
		echo "   to happen: checked against the list of socket/bind/sendmsg/ioctl/setsockopt..."
	fi

	rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
fi

rm -rf "$OBJ"

echo
echo ">> done: $OUT/lib/libusrsctp.a  ($(du -h "$OUT/lib/libusrsctp.a" | cut -f1))"
