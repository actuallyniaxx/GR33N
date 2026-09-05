#!/bin/sh
#
# Prueba las macros que anade ps3-shim, EN LA CONDICION EN QUE HACEN FALTA.
#
#   sh pruebas/correr.sh
#
# La glibc del PC trae CMSG_* y timercmp, asi que compilar contra ella no
# prueba nada: las macros del shim quedarian tapadas por sus #ifndef. Por eso
# hay una consola FALSA en pruebas/consola/, que incluye la del sistema y le
# quita esas macros con #undef. Es lo que creemos que trae PSL1GHT: los
# structs si, las macros no.
#
# Se compila para PowerPC64 big-endian y se ejecuta en qemu, que es lo mas
# cerca de la PS3 que se puede llegar sin encenderla.
#
#   apt install gcc-powerpc64-linux-gnu qemu-user

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SHIM=$(dirname "$HERE")

powerpc64-linux-gnu-gcc -std=gnu99 -Wall -static -o /tmp/t_cmsg \
	"$HERE/t_cmsg.c" \
	-I"$SHIM" -I"$HERE/consola" \
	-DGR33N_HAVE_SS=1 -DGR33N_HAVE_IN6=1 -DGR33N_HAVE_PKTINFO=1

qemu-ppc64 /tmp/t_cmsg
