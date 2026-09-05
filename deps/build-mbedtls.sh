#!/bin/sh
#
# GR33N - construye mbedTLS para PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-mbedtls.sh
#
# Deja $HOME/.gr33n-deps/mbedtls-ps3/ con:
#   include/            cabeceras
#   lib/libmbedtls.a    UNA sola biblioteca con crypto + x509 + tls
#
# TODO ocurre dentro de $HOME, NUNCA en /mnt/c. No es por gusto: /mnt/c es
# DrvFs, el puente de WSL a NTFS, y ahi no se pueden cambiar permisos de
# fichero. Lo primero que hace git al clonar es escribir su config y
# ajustarle el modo, asi que revienta con
#
#   chmod on .git/config.lock failed: Operation not permitted
#
# Y aunque colara, compilar 108 ficheros cruzando ese puente es lento de
# una forma que se nota. El proyecto vive en /mnt/c porque lo editas desde
# Windows; las dependencias no las edita nadie, asi que se quedan del lado
# de Linux.
#
# Por que UNA biblioteca y no las tres que genera mbedTLS: el orden de
# enlace entre libmbedtls, libmbedx509 y libmbedcrypto importa, y
# equivocarse da errores de simbolo indefinido que no dicen nada. Con un
# archivo unico ese problema no existe.
#
# Por que no se usa el sistema de construccion de mbedTLS: compilar 108
# ficheros y meterlos en un archivo son diez lineas que se entienden de un
# vistazo. Pelearse con un Makefile ajeno usando un toolchain cruzado raro
# no son diez lineas.

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
	echo "no encuentro $CC"
	echo "exporta PS3DEV o corrige la ruta"
	exit 1
fi

# --- fuentes ----------------------------------------------------------

if [ ! -d "$SRC" ]; then
	echo ">> clonando $TAG"
	git clone --depth 1 --branch "$TAG" \
	          --recurse-submodules --shallow-submodules \
	          https://github.com/Mbed-TLS/mbedtls.git "$SRC"
else
	echo ">> usando $SRC (ya clonado)"
fi

# --- compilacion ------------------------------------------------------

# -DMBEDTLS_USER_CONFIG_FILE se aplica DESPUES de la configuracion por
# defecto: no reemplaza nada, solo quita lo que la consola no tiene.
CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell"
CFLAGS="$CFLAGS -I$SRC/include -I$HERE"

# Y el gancho del temporizador.
#
# ps3_mbedtls_config.h pone MBEDTLS_TIMING_ALT, y con eso
# include/mbedtls/timing.h hace `#include "timing_alt.h"` en vez de
# declarar sus estructuras. Ese fichero es nuestro y vive aqui.
#
# Lo incluyen library/timing.c y library/entropy_poll.c, o sea que hace
# falta durante la compilacion de mbedTLS y no solo despues.
CFLAGS="$CFLAGS -I$HERE/mbedtls-ps3"

CFLAGS="$CFLAGS -DMBEDTLS_USER_CONFIG_FILE=\"ps3_mbedtls_config.h\""

OBJ="$WORK/.obj-mbedtls"
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

echo ">> compilando con $(basename "$CC")"

n=0
fail=0
for f in "$SRC"/library/*.c; do
	b=$(basename "$f" .c)
	# shellcheck disable=SC2086
	if $CC -c "$f" -o "$OBJ/$b.o" $CFLAGS 2> "$OBJ/$b.err"; then
		n=$((n + 1))
	else
		fail=$((fail + 1))
		echo "   FALLA $b"
		head -5 "$OBJ/$b.err"
	fi
done

echo ">> $n compilados, $fail fallidos"
[ "$fail" -eq 0 ] || { echo "abortando"; exit 1; }

"$AR" rcs "$OUT/lib/libmbedtls.a" "$OBJ"/*.o
cp -r "$SRC/include/mbedtls" "$SRC/include/psa" "$OUT/include/"
cp "$HERE/ps3_mbedtls_config.h" "$OUT/include/"

# timing_alt.h va DENTRO de include/mbedtls/, al lado de timing.h.
#
# No es capricho: timing.h lo pide con comillas -- `#include
# "timing_alt.h"` --, y un include entre comillas se busca PRIMERO en el
# directorio del fichero que lo pide. Poniendolo ahi, cualquiera que use
# esta instalacion -libpeer, GR33N, la prueba de enlace- lo encuentra sin
# tener que anadir un -I mas y sin saber que existe.
cp "$HERE/mbedtls-ps3/timing_alt.h" "$OUT/include/mbedtls/"

# --- que queda sin resolver -------------------------------------------
#
# Ojo, que esto la primera vez lo hice mal: nm --undefined-only sobre un
# ARCHIVO lista los indefinidos de cada objeto, incluidos los que resuelve
# el objeto de al lado. Salian 600 y parecia una catastrofe. Lo que hay
# que mirar es indefinidos MENOS definidos.
#
# Lo esperado: funciones de libc (las trae newlib), inet_pton (libnet) y
# las dos que GR33N pone a proposito. Cualquier otra cosa significa que la
# biblioteca espera algo del sistema operativo que la consola no tiene, y
# es mejor verlo ahora que en el enlace final entre cien errores que no
# dicen nada.

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libmbedtls.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libmbedtls.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	echo
	echo ">> simbolos sin resolver:"
	comm -23 "$WORK/.undef" "$WORK/.def" | sed 's/^/   /'
	rm -f "$WORK/.undef" "$WORK/.def"

	echo
	echo "   Esperado: libc, inet_pton, y mbedtls_hardware_poll +"
	echo "   mbedtls_ms_time, que las escribe GR33N en net_tls.c."

	# --- Y QUE DTLS-SRTP HAYA ENTRADO DE VERDAD -------------------
	#
	# Compilar sin fallos NO demuestra que la opcion este puesta. Si
	# MBEDTLS_SSL_DTLS_SRTP no llegara -- un USER_CONFIG_FILE que no se
	# encuentra, un #undef que lo pisa -- los 108 ficheros compilarian
	# exactamente igual: el codigo de use_srtp simplemente quedaria
	# fuera con su #if, sin una linea de aviso.
	#
	# El fallo saldria mucho mas tarde, al enlazar libpeer, en forma de
	# dos simbolos que no existen y ningun indicio de por que.
	#
	# (Del temporizador no hace falta comprobar nada: si MBEDTLS_TIMING_ALT
	# no hubiera entrado, library/timing.c habria saltado con su propio
	# #error "This module only works on Unix and Windows" y esto no
	# habria llegado hasta aqui. Ahi la prueba es que compilo.)
	echo
	if "$NM" --defined-only "$OUT/lib/libmbedtls.a" 2>/dev/null \
	   | grep -q "mbedtls_ssl_get_dtls_srtp_negotiation_result"; then
		echo ">> DTLS-SRTP: dentro (comprobado en la biblioteca, no supuesto)"
	else
		echo "!! DTLS-SRTP NO ESTA EN LA BIBLIOTECA."
		echo "   MBEDTLS_SSL_DTLS_SRTP no ha llegado a la compilacion, y sin"
		echo "   eso no hay de donde sacar las claves de SRTP: libpeer no va"
		echo "   a enlazar."
		echo
		echo "   Mira que ps3_mbedtls_config.h lo defina y que el"
		echo "   -DMBEDTLS_USER_CONFIG_FILE apunte donde debe."
		exit 1
	fi
fi

rm -rf "$OBJ"

echo
echo ">> listo: $OUT/lib/libmbedtls.a  ($(du -h "$OUT/lib/libmbedtls.a" | cut -f1))"
echo ">> cabeceras en $OUT/include"
echo
echo "   El Makefile de GR33N lo busca solo en esa ruta."
echo "   Si lo mueves:  make MBEDTLS=/otra/ruta"
