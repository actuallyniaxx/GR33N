#!/bin/sh
#
# Tests the macros ps3-shim adds, IN THE CONDITION WHERE THEY'RE NEEDED.
#
#   sh tests/run.sh
#
# The PC's glibc ships CMSG_* and timercmp, so compiling against it proves
# nothing: the shim's macros would be hidden behind their own #ifndef. Hence
# the FAKE console headers in tests/console/, which include the system ones
# and then #undef those macros. That's what we believe PSL1GHT provides: the
# structs yes, the macros no.
#
# It builds for PowerPC64 big-endian and runs under qemu, which is as close
# to a PS3 as you can get without switching one on.
#
#   apt install gcc-powerpc64-linux-gnu qemu-user

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SHIM=$(dirname "$HERE")

powerpc64-linux-gnu-gcc -std=gnu99 -Wall -static -o /tmp/t_cmsg \
	"$HERE/t_cmsg.c" \
	-I"$SHIM" -I"$HERE/console" \
	-DGR33N_HAVE_SS=1 -DGR33N_HAVE_IN6=1 -DGR33N_HAVE_PKTINFO=1

qemu-ppc64 /tmp/t_cmsg
