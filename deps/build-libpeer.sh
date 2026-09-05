#!/bin/sh
#
# GR33N - construye libpeer para PS3 (PSL1GHT / ppu-gcc)
#
#   sh deps/build-libpeer.sh
#
# Deja $HOME/.gr33n-deps/libpeer-ps3/ con:
#   include/peer.h, peer_connection.h
#   lib/libpeer.a
#
# ES LA ULTIMA DE LAS CUATRO. Antes tienen que estar hechas, en este orden:
#
#   sh deps/build-mbedtls.sh    (108 ficheros, y DTLS-SRTP dentro)
#   sh deps/build-libsrtp.sh    (18 ficheros)
#   sh deps/build-usrsctp.sh    (23 ficheros)
#
# LO QUE ES libpeer Y LO QUE NO ES. Son 6653 lineas que pegan entre si mbedTLS
# (DTLS), libSRTP (el cifrado de los paquetes RTP), usrsctp (los canales de
# datos) y un agente de ICE propio. No decodifica video ni reensambla nada:
# eso es de GR33N.

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
# libsrtp-ps3, con "lib" delante: es como lo llama build-libsrtp.sh. Aqui
# ponia srtp-ps3 y el script se plantaba diciendo "corre primero
# build-libsrtp.sh" justo despues de que build-libsrtp.sh acabara bien.
SRTP="$WORK/libsrtp-ps3"
SCTP="$WORK/usrsctp-ps3"

# EL COMMIT, Y NO ES CAPRICHO.
#
# green-nx lo dejo clavado con este aviso: "upstream moves under us (2026-08
# master broke every hunk of libpeer-switch.patch and bumped mbedtls to the
# 4.x layout)". Nuestro parche sale del suyo, asi que hereda el pin. Subirlo
# significa rebasar 41 hunks y volver a probar un stream de verdad.
LIBPEER_REF=9319aa434cb9e893faed0293ba9d2a21eca59c8b

mkdir -p "$WORK"

if [ ! -x "$CC" ]; then
	echo "no encuentro $CC"
	exit 1
fi

# --- las tres de antes -------------------------------------------------

for d in "$MBED" "$SRTP" "$SCTP"; do
	if [ ! -d "$d/include" ]; then
		echo "falta $d"
		echo
		echo "libpeer necesita las otras tres hechas primero:"
		echo "   sh deps/build-mbedtls.sh"
		echo "   sh deps/build-libsrtp.sh"
		echo "   sh deps/build-usrsctp.sh"
		exit 1
	fi
done

# Y que mbedTLS traiga DTLS-SRTP de verdad. Sin eso, dtls_srtp.c de libpeer
# se queda sin las dos funciones de las que salen las claves de SRTP, y el
# error saldria al enlazar sin decir por que.
if [ -x "$NM" ] && [ -f "$MBED/lib/libmbedtls.a" ]; then
	if ! "$NM" --defined-only "$MBED/lib/libmbedtls.a" 2>/dev/null \
	     | grep -q "mbedtls_ssl_get_dtls_srtp_negotiation_result"; then
		echo "!! la libmbedtls.a que hay NO trae DTLS-SRTP."
		echo "   Vuelve a correr deps/build-mbedtls.sh; ahora lo comprueba solo."
		exit 1
	fi
fi

# --- las cabeceras de PSL1GHT ------------------------------------------

PSL_INC=""
PSL_LIB=""
for d in "$PS3DEV/ppu" "$PSL1GHT/ppu" "$PS3DEV/portlibs/ppu"; do
	if [ -f "$d/include/netinet/in.h" ]; then
		PSL_INC="-I$d/include"
		PSL_LIB="-L$d/lib"
		echo ">> cabeceras de PSL1GHT en $d/include"
		break
	fi
done

if [ -z "$PSL_INC" ]; then
	echo "no encuentro netinet/in.h en ninguna ruta de PSL1GHT"
	exit 1
fi

PROBE="$WORK/.probe-libpeer"
mkdir -p "$PROBE"

# --- LA SONDA ----------------------------------------------------------
#
# ENLAZA, no solo compila. Es la leccion de la vuelta de CMSG: en C99,
# llamar a algo que no esta declarado es un AVISO, asi que "compila" no
# quiere decir "existe". Y una cosa por prueba: la primera version de la
# sonda de sockets metia select y FD_ISSET en el mismo programa, fallo, y no
# se pudo saber cual de los dos faltaba.

echo
echo ">> mirando que trae la consola"

LIBS_SONDA="-lnet -lnetctl -lsysmodule -lrt -llv2 -lm"
OPTS_SONDA="-std=gnu99 $PSL_INC -Werror=implicit-function-declaration -Werror=implicit-int"

enlaza() {
	printf '%s\n' "$2" > "$PROBE/p.c"
	# shellcheck disable=SC2086
	if $CC "$PROBE/p.c" -o "$PROBE/p.elf" $OPTS_SONDA $PSL_LIB $LIBS_SONDA \
	   > "$PROBE/p.err" 2>&1; then
		echo "   SI   $1"
		return 0
	else
		echo "   no   $1"
		return 1
	fi
}

# select SE MIRA, PERO YA NO SE USA. Y merece una linea de aviso, porque
# este "SI" es exactamente el que despisto durante dos rondas: select
# EXISTE en PSL1GHT y enlaza sin rechistar. Lo que no se puede es
# llamarla, porque los descriptores de la PS3 valen 0x40000026 y FD_SET
# los convierte en una escritura 64 MB fuera de la pila.
#
# La sonda se queda porque el dia que PSL1GHT lo arregle querremos verlo,
# pero el parche manda los tres sitios de libpeer por netPoll.
SELECT_DEF=""
if ! enlaza "select()" '#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
int main(void){fd_set r; struct timeval tv;
FD_ZERO(&r); tv.tv_sec=0; tv.tv_usec=1000;
return select(1,&r,0,0,&tv);}'; then
	SELECT_DEF="-DGR33N_FALTA_SELECT=1"
	echo "        -> lo pone ps3_net_stubs.c sobre netSelect()"
else
	echo "        (existe, pero da igual: en la PS3 no se puede llamar."
	echo "         libpeer va por netPoll. Ver agent.c en el parche.)"
fi

ISSET_DEF=""
if ! enlaza "FD_ISSET" '#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
int main(void){fd_set r; FD_ZERO(&r); return FD_ISSET(0,&r) ? 1 : 0;}'; then
	ISSET_DEF="-DGR33N_FALTA_FD_ISSET=1"
	echo "        !! FD_ISSET NO ESTA, y libpeer lo usa en agent.c:101."
	echo "           Eso no esta resuelto todavia: pega esta salida."
	exit 1
fi

# El orden de bytes, que es de lo que depende que se lea un solo paquete RTP.
if ! enlaza "<endian.h> con __BYTE_ORDER" "#include <endian.h>
#if !defined(__BYTE_ORDER) || !defined(__BIG_ENDIAN)
#error faltan
#endif
#if __BYTE_ORDER != __BIG_ENDIAN
#error no es big-endian
#endif
int main(void){return 0;}"; then
	echo "        -> lo pone ps3-shim/endian.h (asi que va -I\$SHIM delante)"
fi

# --- LA FAMILIA net* ---------------------------------------------------
#
# El parche pasa TODO socket.c a netSocket/netBind/netSendTo/... porque
# PSL1GHT tiene dos familias de red que no comparten descriptores:
# socket() devuelve 0x4000002D y netPoll contesta POLLNVAL; netSocket()
# devuelve 46 y netPoll espera lo que le pidas. Medido el 2026-09-03.
#
# De las diez, GR33N ya usaba nueve en link.c y net_tls.c. La decima,
# netGetSockName, no la habia usado nadie -- y una funcion de red que no
# existe se nota tarde y mal: sin declaracion es un aviso, no un error, y
# el enlazado en C no comprueba firmas. Asi que se preguntan todas aqui,
# de una en una, y el que falle sale con nombre.
echo
echo ">> la familia net*, que es por donde va ahora socket.c"

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
	echo "!! FALTAN de la familia net*:$FALTAN_NET"
	echo "   socket.c del parche las usa. Si el error de arriba es de"
	echo "   firma y no de simbolo que falta, pegamelo y ajusto la"
	echo "   llamada; si de verdad no existe, hay que rodearla."
	exit 1
fi

# --- fuentes -----------------------------------------------------------

if [ ! -d "$SRC" ]; then
	echo
	echo ">> clonando libpeer"
	git clone --quiet https://github.com/sepfy/libpeer "$SRC"
else
	echo ">> usando $SRC (ya clonado)"
fi

cd "$SRC"

# Al commit clavado, y limpio. Igual que en usrsctp: nadie edita esto a
# mano, asi que devolverlo a como vino no pierde nada, y sin esto un parche
# nuevo no aplica nunca sobre el de la vuelta anterior.
git checkout --quiet "$LIBPEER_REF" 2>/dev/null || {
	git fetch --quiet origin "$LIBPEER_REF" && git checkout --quiet "$LIBPEER_REF"
}
git checkout --quiet -- . 2>/dev/null || true
git clean -qfd 2>/dev/null || true

if git apply --check "$HERE/libpeer-ps3.patch" 2>/dev/null; then
	git apply "$HERE/libpeer-ps3.patch"
	echo ">> parche de PS3 aplicado"
else
	echo "!! el parche no aplica. Pega esto:"
	git apply --verbose "$HERE/libpeer-ps3.patch" 2>&1 | head -20
	exit 1
fi

cd - > /dev/null

# --- compilacion --------------------------------------------------------

CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell"

# El shim PRIMERO: de ahi sale <endian.h>, que es de donde libpeer saca
# __BYTE_ORDER para elegir la disposicion de RtpHeader y RtcpHeader. Si eso
# se resolviera mal, no habria error: habria paquetes RTP mal leidos.
CFLAGS="$CFLAGS -I$SHIM"

CFLAGS="$CFLAGS -I$SRC/src"
CFLAGS="$CFLAGS -I$MBED/include -I$SRTP/include -I$SCTP/include"

# LA MISMA CONFIGURACION DE mbedTLS CON LA QUE SE COMPILO mbedTLS.
#
# Esto faltaba y se noto en el acto: dtls_srtp.c no encontraba
# mbedtls_ssl_srtp_profile, MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
# mbedtls_ssl_conf_dtls_srtp_protection_profiles ni
# mbedtls_ssl_get_dtls_srtp_negotiation_result -- diez errores-- justo
# despues de que build-mbedtls.sh confirmara que DTLS-SRTP SI estaba en la
# biblioteca.
#
# Las dos cosas eran ciertas a la vez, y ahi esta el asunto: la biblioteca
# tenia DTLS-SRTP porque se construyo con ps3_mbedtls_config.h, y libpeer no
# lo veia porque se compilaba SIN el, leyendo las cabeceras de mbedTLS en su
# estado por defecto, donde MBEDTLS_SSL_DTLS_SRTP viene comentado.
#
# Y NO ES SOLO CUESTION DE FUNCIONES QUE FALTAN. La configuracion de mbedTLS
# tambien guarda CAMPOS DE ESTRUCTURA tras #if. libpeer lleva un
# mbedtls_ssl_context entero dentro de DtlsSrtp: compilarlo contra una
# configuracion y enlazarlo contra otra da estructuras de tamaños distintos
# a cada lado del enlace, que es corrupcion de memoria sin un solo aviso.
# Aqui el fallo fue ruidoso por suerte; la version muda de este mismo error
# es la que da miedo.
#
# REGLA: todo lo que compile contra estas cabeceras lleva este -D. El
# Makefile de GR33N ya lo hace (linea 72); build-libsrtp.sh no lo hacia y
# tambien se le ha puesto.
CFLAGS="$CFLAGS -DMBEDTLS_USER_CONFIG_FILE=\"ps3_mbedtls_config.h\""

CFLAGS="$CFLAGS $PSL_INC"

CFLAGS="$CFLAGS -DGR33N_PS3=1 $SELECT_DEF $ISSET_DEF"

# Sin señalizacion: fuera coreHTTP, coreMQTT y cJSON.
#
# Eso es para el modo en el que libpeer se conecta a un broker MQTT y
# negocia el SDP por su cuenta. Nosotros ya tenemos la sesion montada
# -session.c habla con el API v5 de xCloud- y le damos la oferta y la
# respuesta a mano. peer_signaling.c y ssl_transport.c estan enteros dentro
# de un #ifndef, asi que se compilan a nada.
CFLAGS="$CFLAGS -DDISABLE_PEER_SIGNALING=1"

# Los canales de datos van por usrsctp y no por el SCTP propio de libpeer.
# Ver claude/webrtc-portado.md: el suyo no retransmite, y el input de xCloud
# deja de aplicarse en cuanto el servidor ve un hueco en la numeracion.
CFLAGS="$CFLAGS -DCONFIG_USE_USRSCTP=1"

# Los logs de libpeer, a peer_log(), que GR33N manda al servidor de
# depuracion. Arrancando desde el XMB no hay TTY: lo que no llega al PC no
# existe.
#
# Y NIVEL 2 (INFO), NO DEBUG. En DEBUG libpeer escribe una linea POR PAQUETE
# RTP. A 60 fps eso son cientos por segundo por un socket UDP: ahoga el log y
# atasca el hilo. Es el aviso de green-nx, que se lo comio.
CFLAGS="$CFLAGS -DLOG_REDIRECT=1 -DLOG_LEVEL=2"

# Por lo mismo que en libSRTP y usrsctp: se leen palabras de 32 bits desde
# buffers de char por todas partes.
CFLAGS="$CFLAGS -fno-strict-aliasing"

# LA OPCION QUE YA SE COBRO DOS FALLOS EN usrsctp.
#
# Sin esto, una macro o una funcion que no existe se convierte en una
# llamada implicita, que en C99 es un aviso. Y cuando lo que falta devolvia
# un PUNTERO, en un binario de 32 bits el int que el compilador se inventa
# mide lo mismo: compila, enlaza, arranca, y escribe en una direccion
# truncada dentro de la consola.
CFLAGS="$CFLAGS -Werror=implicit-function-declaration -Werror=implicit-int"

CFLAGS="$CFLAGS -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function"

# --- Y QUE LA CONFIGURACION LLEGUE DE VERDAD ---------------------------
#
# Con las opciones ya montadas, se comprueba en el sitio exacto donde
# importa. Que libmbedtls.a tenga DTLS-SRTP y que libpeer LO VEA son dos
# cosas distintas, y esta ronda salieron distintas.
#
# Diez errores de "unknown type name" que no mencionan la configuracion por
# ningun lado se convierten aqui en una linea que dice lo que pasa.
printf '#include <mbedtls/ssl.h>\n#if !defined(MBEDTLS_SSL_DTLS_SRTP)\n#error no llega\n#endif\nint main(void){return 0;}\n' > "$PROBE/c.c"
# shellcheck disable=SC2086
if $CC -c "$PROBE/c.c" -o "$PROBE/c.o" $CFLAGS > /dev/null 2>&1; then
	echo ">> la configuracion de mbedTLS llega a libpeer (DTLS-SRTP visible)"
else
	echo "!! LAS CABECERAS DE mbedTLS NO TRAEN DTLS-SRTP."
	echo
	echo "   La biblioteca puede tenerlo y aun asi pasar esto: si el"
	echo "   -DMBEDTLS_USER_CONFIG_FILE no llega, las cabeceras se leen en"
	echo "   su estado por defecto, donde MBEDTLS_SSL_DTLS_SRTP viene"
	echo "   comentado."
	echo
	echo "   Mira que exista $MBED/include/ps3_mbedtls_config.h"
	echo "   (lo copia build-mbedtls.sh al instalar)."
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

# SE MONTA A UN LADO Y SE CAMBIA AL FINAL.
#
# Antes esto hacia rm -rf "$OUT" aqui arriba, y una compilacion fallida
# dejaba a GR33N sin peer.h: el make siguiente moria con "peer.h: No such
# file or directory", que no tiene NADA que ver con el fallo de verdad y
# manda a buscar donde no es. Pasó tal cual el 2026-09-01 con utils.c.
#
# Una dependencia que falla no puede llevarse por delante la instalacion
# que ya funcionaba. Se compila en un directorio aparte y solo se pone en
# su sitio cuando ha salido todo bien, comprobaciones incluidas.
FINAL="$OUT"
OUT="$WORK/.libpeer-ps3-en-obras"
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

echo
echo ">> compilando con $(basename "$CC")"

n=0
fail=0
for f in $FILES; do
	b=$(basename "$f" .c)

	case "$f" in
	/*) ruta="$f" ;;
	*)  ruta="$SRC/$f" ;;
	esac

	if [ ! -f "$ruta" ]; then
		echo "   NO EXISTE $ruta"
		fail=$((fail + 1))
		continue
	fi

	# shellcheck disable=SC2086
	if $CC -c "$ruta" -o "$OBJ/$b.o" $CFLAGS 2> "$OBJ/$b.err"; then
		n=$((n + 1))
	else
		fail=$((fail + 1))
		echo "   FALLA $f"
		grep -E "error:" "$OBJ/$b.err" | sed 's/^/     /'
	fi
done

echo ">> $n compilados, $fail fallidos"

if [ "$fail" -ne 0 ]; then
	echo
	echo "   Los ficheros de error completos estan en $OBJ/*.err"
	echo "   Pega esta salida y seguimos."
	exit 1
fi

"$AR" rcs "$OUT/lib/libpeer.a" "$OBJ"/*.o
cp "$SRC/src/peer.h" "$SRC/src/peer_connection.h" "$OUT/include/"
[ -f "$SRC/src/peer_signaling.h" ] && cp "$SRC/src/peer_signaling.h" "$OUT/include/"

# --- que queda sin resolver ---------------------------------------------

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libpeer.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libpeer.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	comm -23 "$WORK/.undef" "$WORK/.def" > "$WORK/.falta"

	echo
	echo ">> simbolos sin resolver:"
	sed 's/^/   /' "$WORK/.falta"

	echo
	echo "   Esperado, y todo se resuelve al enlazar el EBOOT:"
	echo
	echo "     libc de newlib       memcpy, malloc, printf, usleep..."
	echo "     la red de la consola socket, bind, connect, select,"
	echo "                          sendto, inet_pton, netGetHostByName"
	echo "     mbedtls_*            libmbedtls.a"
	echo "     srtp_*               libsrtp2.a"
	echo "     usrsctp_*            libusrsctp.a"
	echo "     getifaddrs y cia.    ps3_stubs.c, dentro de libusrsctp.a"
	echo "     mbedtls_timing_*     source/dtls_timer.c de GR33N"
	echo
	echo "   Y UNO que todavia no existe y hay que escribir:"
	echo
	echo "     peer_log   los logs de libpeer al log remoto. Con"
	echo "                LOG_REDIRECT=1, libpeer manda ahi todo lo que"
	echo "                diria por stdout, y desde el XMB no hay TTY:"
	echo "                lo que no llega al PC no existe. La firma esta"
	echo "                en src/utils.h:24."

	# getaddrinfo NO puede aparecer: si sale, el parche de
	# ports_resolve_addr no ha entrado y el EBOOT no va a enlazar.
	if grep -qx "getaddrinfo" "$WORK/.falta"; then
		echo
		echo "!! SALE getaddrinfo, y PSL1GHT no lo tiene."
		echo "   El parche reescribe ports_resolve_addr() sobre"
		echo "   netGetHostByName; si el simbolo sigue ahi, esa parte del"
		echo "   parche no se ha aplicado."
		rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
		exit 1
	fi

	# select NO PUEDE APARECER. Y esta comprobacion ha cambiado de
	# sentido dos veces, asi que conviene contar por que.
	#
	# La primera version se quejaba en cuanto veia select sin resolver.
	# Estaba mal: si la consola lo trae, select sale sin resolver igual
	# que socket o sendto y se arregla al enlazar el EBOOT. La segunda
	# version solo miraba si la sonda habia dicho que faltaba.
	#
	# Ahora la respuesta buena es OTRA: select no debe salir NUNCA,
	# resuelto o sin resolver, porque en la PS3 no se puede llamar. Un
	# descriptor de aqui vale 0x40000026 y FD_SET(0x40000026, ...) indexa
	# el elemento 16.777.216 de un array de 16 -- una escritura 64 MB
	# mas alla de la pila, y la consola se para en seco. Medido el
	# 2026-09-01 con WEBRTC_DEBUG.
	#
	# El parche cambia los tres sitios (agent.c, mdns.c,
	# ssl_transport.c) por netPoll, que mete el descriptor en un int y no
	# tiene tope. Asi que si select asoma aqui, es que alguno de los tres
	# no se ha parcheado -- o que libpeer ha crecido un cuarto.
	if grep -qx "select" "$WORK/.falta" || grep -qx "select" "$WORK/.def"; then
		echo
		echo "!! SALE select, Y EN LA PS3 ESO CUELGA LA CONSOLA."
		echo "   Los sitios conocidos son agent.c, mdns.c y"
		echo "   ssl_transport.c, y el parche los cambia por netPoll."
		echo "   Si sale de todas formas, busca quien lo llama con:"
		echo "     ppu-nm -A $OUT/lib/libpeer.a | grep ' U select'"
		rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
		exit 1
	fi

	rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
fi

rm -rf "$OBJ"

# Todo ha salido bien: ahora si se cambia la instalacion buena.
rm -rf "$FINAL"
mv "$OUT" "$FINAL"
OUT="$FINAL"

echo
echo ">> listo: $OUT/lib/libpeer.a  ($(du -h "$OUT/lib/libpeer.a" | cut -f1))"
echo
echo "   Las cuatro bibliotecas estan. Lo siguiente es cablearlas al"
echo "   Makefile de GR33N y escribir la capa que las usa."
