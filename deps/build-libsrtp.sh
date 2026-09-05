#!/bin/sh
#
# GR33N - construye libSRTP para PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-libsrtp.sh
#
# Deja $HOME/.gr33n-deps/libsrtp-ps3/ con:
#   include/            cabeceras
#   lib/libsrtp2.a
#
# Misma forma que build-mbedtls.sh y por las mismas razones: todo dentro de
# $HOME y nunca en /mnt/c (DrvFs no deja cambiar permisos y git revienta al
# clonar), y compilacion a mano en vez de pelearse con cmake cruzado.
#
# DEPENDE DE mbedTLS. libSRTP puede traer su propio AES y su propio SHA1,
# pero se le dice que use el de mbedTLS, que es el que lleva semanas
# funcionando en esta consola. Meter una segunda implementacion de AES sin
# probar, en la plataforma donde el orden de bytes ya nos ha mordido, no
# tiene ningun sentido. Asi que build-mbedtls.sh primero.
#
# LO QUE HAY QUE MIRAR AL FINAL es la lista de simbolos sin resolver. Si
# ahi sale algo que no sea libc o psa_*, libSRTP espera algo del sistema
# que la consola no tiene, y es mejor verlo ahora que entre cien errores de
# enlace que no dicen nada.
#
# ESTO YA SE HA PROBADO, aunque no con ppu-gcc: los 18 ficheros compilan
# limpios para PowerPC64 big-endian con el cruzado de Debian, sin un solo
# aviso ni con -Wcast-align. No es la misma libc ni el mismo sistema, asi
# que aqui pueden salir cosas nuevas -- pero el orden de bytes, que era el
# riesgo de verdad, es el mismo y esta comprobado.
#
# Y en efecto salio una: el cruzado tiene <netinet/in.h> en la ruta por
# defecto porque usa glibc, y ppu-gcc no. Ver el bloque de PSL_INC mas
# abajo. Dos ficheros de dieciocho compilaron -- justo los dos que no
# incluyen datatypes.h.

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
	echo "no encuentro $CC"
	echo "exporta PS3DEV o corrige la ruta"
	exit 1
fi

if [ ! -d "$MBED/include" ]; then
	echo "falta mbedTLS en $MBED"
	echo "corre primero:  sh build-mbedtls.sh"
	exit 1
fi

# --- fuentes ----------------------------------------------------------

# EL COMMIT CLAVADO, Y ESTO SE DESCUBRIO TARDE.
#
# Antes aqui habia `git clone --depth 1` a secas, o sea MASTER. Y master de
# libSRTP es hoy la 3.0.0, que es otra API: srtp_crypto_policy_set_rtp_default
# y srtp_crypto_policy_set_rtcp_default -- las dos que llama dtls_srtp.c de
# libpeer para montar la politica de cifrado-- ya no existen ahi.
#
# Lo peor es como se manifestaba: los 18 ficheros compilaban perfectamente y
# la biblioteca quedaba hecha. La 3.0 compila igual de bien que la 2.x; solo
# que no tiene esas funciones. El fallo habria salido al final del todo, al
# enlazar libpeer, en forma de simbolos que no existen y ninguna pista de que
# el problema era la VERSION.
#
# 90d05bf es la 2.4.2, y no es una eleccion nuestra: es el submodulo que
# libpeer clava en su propio arbol (third_party/libsrtp del commit 9319aa4).
# O sea la version contra la que libpeer esta escrito y probado.
#
# Y no puede ir con --depth 1: eso solo trae la punta de la rama, y este
# commit no lo es.
LIBSRTP_REF=90d05bf8980d16e4ac3f16c19b77e296c4bc207b

if [ ! -d "$SRC" ]; then
	echo ">> clonando libsrtp"
	git clone --quiet https://github.com/cisco/libsrtp.git "$SRC"
fi

echo ">> libsrtp en 2.4.2 ($LIBSRTP_REF)"
(
	cd "$SRC"
	git checkout --quiet "$LIBSRTP_REF" 2>/dev/null || {
		git fetch --quiet origin "$LIBSRTP_REF" && git checkout --quiet "$LIBSRTP_REF"
	}
	git checkout --quiet -- . 2>/dev/null || true
) || {
	echo "!! no he podido dejar libsrtp en $LIBSRTP_REF."
	echo "   Si el clon de antes era --depth 1, borralo y vuelve a correr:"
	echo "     rm -rf $SRC"
	exit 1
}

# Y se comprueba que la version es la que se cree, en vez de darlo por hecho.
if ! grep -rq "srtp_crypto_policy_set_rtp_default" "$SRC/include/srtp.h"; then
	echo "!! esta libsrtp no trae srtp_crypto_policy_set_rtp_default."
	echo "   Es la API que usa dtls_srtp.c de libpeer. Si esto salta, el"
	echo "   checkout no ha ido a la 2.4.2."
	exit 1
fi

# --- el config.h -------------------------------------------------------
#
# libSRTP lo espera de autoconf o cmake. Aqui va a mano, y ahi dentro esta
# WORDS_BIGENDIAN, que es la linea de la que depende que esto funcione.
# Ver el comentario largo de ps3_srtp_config.h.

mkdir -p "$WORK/.srtp-cfg"
cp "$HERE/ps3_srtp_config.h" "$WORK/.srtp-cfg/config.h"

# --- compilacion ------------------------------------------------------

# LAS CABECERAS DE PSL1GHT NO ESTAN EN LA RUTA POR DEFECTO DE ppu-gcc.
#
# libSRTP incluye <netinet/in.h> para ntohs, y esa cabecera SI existe -- el
# link.c de GR33N lleva semanas usandola. Lo que pasa es que vive en el
# directorio de PSL1GHT, y quien lo mete en la ruta es $(LIBPSL1GHT_INC)
# desde ppu_rules, o sea el Makefile del proyecto. Compilando a mano, como
# aqui, no lo pone nadie.
#
# Se busca en vez de darla por sabida: si algun dia PSL1GHT cambia de sitio,
# esto lo dice en vez de fallar con dieciseis errores identicos.
PSL_INC=""
for d in "$PS3DEV/ppu/include" "$PSL1GHT/ppu/include" \
         "$PS3DEV/portlibs/ppu/include"; do
	if [ -f "$d/netinet/in.h" ]; then
		PSL_INC="-I$d"
		echo ">> cabeceras de PSL1GHT en $d"
		break
	fi
done

if [ -z "$PSL_INC" ]; then
	echo "no encuentro netinet/in.h en ninguna ruta de PSL1GHT."
	echo "Buscado en:"
	echo "   $PS3DEV/ppu/include"
	echo "   $PSL1GHT/ppu/include"
	echo "   $PS3DEV/portlibs/ppu/include"
	echo
	echo "Mira donde esta con:  find \$PS3DEV -name in.h -path '*netinet*'"
	exit 1
fi

CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell -DHAVE_CONFIG_H"
CFLAGS="$CFLAGS -I$WORK/.srtp-cfg"
CFLAGS="$CFLAGS -I$SRC/include -I$SRC/crypto/include -I$SRC"
CFLAGS="$CFLAGS -I$MBED/include"

# LA MISMA CONFIGURACION DE mbedTLS CON LA QUE SE COMPILO mbedTLS.
#
# Esto faltaba, y no se noto porque libSRTP tuvo suerte: solo usa
# mbedtls_aes_*, mbedtls_gcm_* y mbedtls_md_*, que estan encendidas por
# defecto. Pero la suerte no es un diseño.
#
# Sin este -D, las cabeceras de mbedTLS se leen en su estado POR DEFECTO, que
# no es el estado con el que se construyo libmbedtls.a. Y la configuracion de
# mbedTLS no solo enciende y apaga funciones: hay campos de estructura
# guardados por #if. Compilar contra una configuracion y enlazar contra otra
# da tamaños de estructura distintos a cada lado, que es corrupcion de
# memoria en silencio -- exactamente el fallo del sockaddr_conn de usrsctp,
# por otra puerta.
#
# En libpeer si se noto, y a lo grande: dtls_srtp.c no encontraba
# mbedtls_ssl_srtp_profile ni ninguna de las constantes de SRTP, porque
# MBEDTLS_SSL_DTLS_SRTP lo enciende ESTE fichero. El sintoma fue ruidoso ahi;
# aqui habria sido mudo.
#
# ps3_mbedtls_config.h lo copia build-mbedtls.sh dentro de $MBED/include, o
# sea que el -I de arriba ya lo pone en la ruta.
CFLAGS="$CFLAGS -DMBEDTLS_USER_CONFIG_FILE=\"ps3_mbedtls_config.h\""

CFLAGS="$CFLAGS $PSL_INC"

# Aliasing estricto FUERA. libSRTP lee palabras de 32 bits desde buffers de
# char en varios sitios, que es exactamente lo que GCC da por imposible con
# -O2. En x86 el codigo generado sale bien de casualidad; aqui no vamos a
# depender de la casualidad.
CFLAGS="$CFLAGS -fno-strict-aliasing"

# Y que avise si algun cast deja un puntero peor alineado de lo que promete.
CFLAGS="$CFLAGS -Wcast-align"

# Los ficheros, en la misma seleccion que hace CMakeLists.txt con
# ENABLE_MBEDTLS: la criptografia sale de mbedTLS, no de la propia libSRTP.
# aes.c, aes_icm.c, hmac.c y sha1.c NO entran a proposito.
# srtp/srtp_policy.c NO ESTA: es de la 3.0, que partio srtp.c en dos. En la
# 2.4.2 -- que es la que clava libpeer y por tanto la que compilamos-- todo
# eso vive dentro de srtp/srtp.c.
#
# Lo que NO entra, y es a proposito: aes.c, aes_icm.c, hmac.c y sha1.c son
# la criptografia propia de libSRTP, y aqui la pone mbedTLS (MBEDTLS 1 en
# ps3_srtp_config.h). Los ficheros _nss y _ossl son las otras dos alternativas.
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

echo ">> compilando con $(basename "$CC")"

n=0
fail=0
for f in $FILES; do
	b=$(echo "$f" | tr '/' '_' | sed 's/\.c$//')
	if [ ! -f "$SRC/$f" ]; then
		echo "   NO EXISTE $f  (la lista de arriba se ha quedado atras"
		echo "                  respecto a esta version de libSRTP)"
		fail=$((fail + 1))
		continue
	fi
	# shellcheck disable=SC2086
	if $CC -c "$SRC/$f" -o "$OBJ/$b.o" $CFLAGS 2> "$OBJ/$b.err"; then
		n=$((n + 1))
		# Los avisos se enseñan aunque compile: -Wcast-align sobre esta
		# plataforma es informacion, no ruido.
		if [ -s "$OBJ/$b.err" ]; then
			echo "   avisos en $f:"
			head -6 "$OBJ/$b.err" | sed 's/^/     /'
		fi
	else
		fail=$((fail + 1))
		echo "   FALLA $f"
		head -8 "$OBJ/$b.err" | sed 's/^/     /'
	fi
done

echo ">> $n compilados, $fail fallidos"
[ "$fail" -eq 0 ] || { echo "abortando"; exit 1; }

"$AR" rcs "$OUT/lib/libsrtp2.a" "$OBJ"/*.o
cp -r "$SRC/include/"*.h "$OUT/include/"
cp "$HERE/ps3_srtp_config.h" "$OUT/include/"

# Y LAS MISMAS OTRA VEZ, DENTRO DE srtp2/.
#
# libpeer las incluye asi: `#include <srtp2/srtp.h>`. Ese subdirectorio es
# como se instala libSRTP de verdad -- lo hace su propio `make install`, y
# es donde lo buscan todos los que la usan-- pero aqui compilamos a mano y
# nadie lo estaba creando. Salia "srtp2/srtp.h: No such file or directory"
# en cuatro ficheros de libpeer.
#
# Se copian en los dos sitios en vez de mover: quien incluya <srtp.h> a
# secas -- como hace el propio libSRTP por dentro-- lo sigue encontrando.
mkdir -p "$OUT/include/srtp2"
cp "$SRC/include/"*.h "$OUT/include/srtp2/"

# --- que queda sin resolver -------------------------------------------
#
# Indefinidos MENOS definidos, no indefinidos a secas: nm sobre un ARCHIVO
# lista los indefinidos de cada objeto, incluidos los que resuelve el
# objeto de al lado. Esa leccion ya la pagamos con mbedTLS.

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libsrtp2.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libsrtp2.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	echo
	echo ">> simbolos sin resolver:"
	comm -23 "$WORK/.undef" "$WORK/.def" | sed 's/^/   /'
	rm -f "$WORK/.undef" "$WORK/.def"

	echo
	echo "   Esperado:"
	echo "     - libc: memcpy, memset, calloc, free, strlen, rand, clock,"
	echo "       exit, sscanf, vfprintf... todo eso lo trae newlib."
	echo "     - mbedtls_aes_*, mbedtls_gcm_* y mbedtls_md_*, que llegan"
	echo "       al enlazar el EBOOT contra libmbedtls.a."
	echo
	echo "   OJO, QUE ESTO CAMBIO AL CLAVAR LA 2.4.2. Aqui ponia que"
	echo "   saldrian trece simbolos psa_*, y era verdad con la 3.0.0:"
	echo "   aquella habla con mbedTLS por la API nueva (PSA). La 2.4.2"
	echo "   usa la CLASICA -- mbedtls_aes_crypt_ctr, mbedtls_gcm_setkey,"
	echo "   mbedtls_md_hmac_starts--, que es otra lista entera de"
	echo "   simbolos. El mensaje se quedo describiendo la version que ya"
	echo "   no compilamos."
	echo
	echo "   Cualquier OTRA cosa hay que mirarla antes de seguir."

	# --- Y QUE mbedTLS TRAIGA ESOS SIMBOLOS, COMPROBADO -----------
	#
	# Que salgan aqui sin resolver es normal: se resuelven al enlazar el
	# EBOOT. Lo que NO es normal es darlo por hecho. Si
	# ps3_mbedtls_config.h apagara MBEDTLS_GCM_C o MBEDTLS_MD_C -- hoy no
	# lo hace, pero es exactamente el tipo de recorte que se hace un dia
	# para ahorrar sitio-- estos quedarian sin resolver hasta el ultimo
	# enlace, entre otros cien errores.
	#
	# La comprobacion cuesta un nm y se hace ahora.
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
			echo ">> la API clasica de mbedTLS esta en libmbedtls.a"
			echo "   (aes, gcm y md: comprobado, no supuesto)"
		else
			echo "!! libmbedtls.a NO define:$sin"
			echo
			echo "   libSRTP 2.4.2 los necesita. Mira que"
			echo "   ps3_mbedtls_config.h no haya apagado MBEDTLS_AES_C,"
			echo "   MBEDTLS_GCM_C o MBEDTLS_MD_C."
			exit 1
		fi
	fi
fi

rm -rf "$OBJ"

echo
echo ">> listo: $OUT/lib/libsrtp2.a  ($(du -h "$OUT/lib/libsrtp2.a" | cut -f1))"
echo ">> cabeceras en $OUT/include"
