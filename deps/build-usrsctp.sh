#!/bin/sh
#
# GR33N - construye usrsctp para PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-usrsctp.sh
#
# Deja $HOME/.gr33n-deps/usrsctp-ps3/ con:
#   include/usrsctp.h
#   lib/libusrsctp.a
#
# POR QUE ENTRA usrsctp EN EL PROYECTO. El input de xCloud va por cuatro
# canales de datos SCTP -control, input, message, chat-, todos ORDENADOS y
# FIABLES, y el protocolo de input v1 manda el estado absoluto del mando
# numerado por una secuencia: el servidor deja de aplicarlo en cuanto ve un
# hueco. libpeer trae un SCTP propio de 733 lineas que NO retransmite (ante
# un hueco hace `tsn = cumulative_tsn_ack + 1` con la logica de verdad
# comentada al lado), asi que no sirve. Ver claude/webrtc-portado.md.
#
# LO PRIMERO QUE HACE ESTE SCRIPT NO ES COMPILAR: es averiguar si PSL1GHT
# trae pthreads, porque de eso depende si esto son 114 lineas de parche o
# quinientas.
#
# usrsctp quiere pthread_mutex_t, pthread_rwlock_t, pthread_cond_t y
# pthread_t (sctp_os_userspace.h:299-304). Si la consola los trae, se
# compila y ya. Si no, hay que escribir una capa que los traduzca a los
# hilos de lv2 -sysMutexCreate, sysCondCreate, sysThreadCreate-, y eso es
# otro trabajo. La sonda de abajo lo dice en dos segundos en vez de
# descubrirlo entre veintitres ficheros de errores.
#
# LO DEMAS YA ESTA PROBADO, aunque no con ppu-gcc: los 23 ficheros compilan
# limpios para PowerPC64 big-endian con el cruzado de Debian. Los unicos
# errores que salieron alli fueron por sa_len -- la glibc del PC no lo
# tiene y la newlib de PSL1GHT si-, o sea justo lo contrario del problema
# que tendriamos aqui.

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
	echo "no encuentro $CC"
	exit 1
fi

# --- las cabeceras de PSL1GHT ------------------------------------------
#
# No estan en la ruta por defecto de ppu-gcc: las mete $(LIBPSL1GHT_INC)
# desde ppu_rules, o sea el Makefile del proyecto. Compilando a mano no las
# pone nadie, y eso ya nos costo una vuelta con libSRTP.

PSL_INC=""
for d in "$PS3DEV/ppu/include" "$PSL1GHT/ppu/include" \
         "$PS3DEV/portlibs/ppu/include"; do
	if [ -f "$d/netinet/in.h" ]; then
		PSL_INC="-I$d"
		PSL_DIR="$d"
		echo ">> cabeceras de PSL1GHT en $d"
		break
	fi
done

if [ -z "$PSL_INC" ]; then
	echo "no encuentro netinet/in.h en ninguna ruta de PSL1GHT."
	echo "Mira donde esta con:  find \$PS3DEV -name in.h -path '*netinet*'"
	exit 1
fi

# --- LA SONDA ----------------------------------------------------------

echo
echo ">> mirando que trae la consola"

PROBE="$WORK/.probe-usrsctp"
mkdir -p "$PROBE"

# -Werror=implicit-function-declaration NO ES DECORACION: SIN ESO LA SONDA
# MIENTE.
#
# La vuelta pasada esta sonda dijo que la consola traia CMSG_DATA, CMSG_SPACE
# y CMSG_LEN. No las trae. El programa de prueba era:
#
#     struct cmsghdr c; (void)CMSG_DATA(&c);
#     return (int)(CMSG_SPACE(4)+CMSG_LEN(4));
#
# ...y eso COMPILA aunque las macros no existan, porque en C99 llamar a algo
# no declarado es un aviso: el compilador se inventa `int CMSG_DATA()` y
# sigue. La sonda miraba el codigo de salida, veia cero y decia SI.
#
# Es la tercera vez esta semana que una sonda mide donde no es -antes fue
# BYTE_ORDER preguntando por una cabecera que usrsctp no incluye, y
# sys/time.h preguntandole al sistema de ficheros en vez de al compilador-,
# y la que mas gracia tiene, porque la sonda existe precisamente para no
# suponer. Con estas dos opciones, el aviso pasa a ser el error que siempre
# debio ser y la respuesta es de fiar.
PEDANTE="-Werror=implicit-function-declaration -Werror=implicit-int"

# LAS DE pthreads TIENEN QUE ENLAZAR, NO SOLO COMPILAR.
#
# Esta sonda dijo que si a las cinco preguntas de pthreads, y era mentira.
# Compilaba con -c, o sea que lo que contestaba era "las cabeceras declaran
# pthread_mutex_lock" -- verdad, y no la pregunta. La pregunta es si existe
# el SIMBOLO, y eso solo lo sabe el enlazador.
#
# El fallo aparecio al final del todo, enlazando el EBOOT, en forma de mil
# lineas de "undefined reference to pthread_mutex_lock" desde libusrsctp.a.
# Con el trabajo entero ya montado encima.
#
# Es la misma leccion de CMSG -- "compila" no quiere decir "existe"--
# aplicada a la pregunta mas cara de todo el port, que es justo la que este
# script decia estar contestando.
# -lpthread va DELANTE de -lnet, igual que en el LIBS del Makefile: si
# la sonda no enlaza con la misma linea que el EBOOT, no esta midiendo
# lo mismo, y una sonda que no mide lo mismo miente.
LIBS_SONDA="-lpthread -lnet -lnetctl -lsysmodule -lrt -llv2 -lm"
PSL_LIB=""
[ -n "$PSL_DIR" ] && PSL_LIB="-L$(dirname "$PSL_DIR")/lib"

probar() {
	nombre=$1
	codigo=$2
	solo_compilar=$3      # no vacio = basta con compilar

	printf '%s\n' "$codigo" > "$PROBE/p.c"

	if [ -n "$solo_compilar" ]; then
		# shellcheck disable=SC2086
		if $CC -c "$PROBE/p.c" -o "$PROBE/p.o" -std=gnu99 $PEDANTE \
		   $PSL_INC 2> "$PROBE/p.err"; then
			echo "   SI   $nombre"
			return 0
		fi
	else
		# shellcheck disable=SC2086
		if $CC "$PROBE/p.c" -o "$PROBE/p.elf" -std=gnu99 $PEDANTE \
		   $PSL_INC $PSL_LIB $LIBS_SONDA 2> "$PROBE/p.err"; then
			echo "   SI   $nombre"
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

probar "sockaddr con sa_len (familia BSD)" '#include <netinet/in.h>
int main(void){struct sockaddr_in a; a.sin_len=0; return a.sin_len;}' 1 || falta=1

# ESTA NO CUENTA PARA `falta`: no es un requisito, es una pregunta.
#
# PSL1GHT define struct iovec -- lo mete algo que incluye su sys/socket.h --
# pero NO tiene el fichero sys/uio.h. Esa combinacion es rara y hay que
# preguntarla, porque desde el preprocesador no se puede saber si un struct
# ya existe, y PSL1GHT no lo marca con ninguna de las macros habituales.
IOVEC_DEF=""
if probar "struct iovec (ya la trae la consola)" '#include <sys/socket.h>
int main(void){struct iovec v; v.iov_base=0; v.iov_len=0;
return (int)v.iov_len;}' 1; then
	IOVEC_DEF="-DGR33N_HAVE_IOVEC=1"
fi

# Y lo mismo con IPv6. Tampoco cuenta para `falta`: la PS3 no habla IPv6 y
# eso esta bien. Pero usrsctp declara campos de tipo struct in6_addr y
# struct sockaddr_in6 SIN guardarlos tras INET6 (user_inpcb.h:72 y :77,
# usrsctp.h:152), dentro de uniones que se reservan enteras. El tipo tiene
# que existir aunque no se use nunca; compilar con -DINET a secas no evita
# esas lineas, solo el codigo que las mira.
IN6_DEF=""
if probar "struct in6_addr (ya la trae la consola)" '#include <netinet/in.h>
int main(void){struct in6_addr a; struct sockaddr_in6 s;
(void)a;(void)s;return 0;}' 1; then
	IN6_DEF="-DGR33N_HAVE_IN6=1"
fi

# Y sockaddr_storage, que usrsctp declara en sctp_uio.h:348 y :574 dentro
# de estructuras que se reservan enteras.
SS_DEF=""
if probar "struct sockaddr_storage (ya la trae)" '#include <sys/socket.h>
int main(void){struct sockaddr_storage a;(void)a;return 0;}' 1; then
	SS_DEF="-DGR33N_HAVE_SS=1"
fi

# Y la ultima: struct in_pktinfo, que solo hace falta si la consola define
# IP_PKTINFO. Si no define ni eso ni IP_RECVDSTADDR, user_recv_thread.c se
# planta con un #error; la envoltura de netinet/in.h declara la de BSD para
# ese caso.
PKT_DEF=""
if probar "struct in_pktinfo (ya la trae)" '#include <netinet/in.h>
int main(void){struct in_pktinfo a;(void)a;return 0;}' 1; then
	PKT_DEF="-DGR33N_HAVE_PKTINFO=1"
fi

echo

if [ "$falta" -ne 0 ]; then
	cat <<'FIN'
>> FALTA ALGO, y eso cambia el plan.

   usrsctp usa pthreads directamente en sctp_os_userspace.h (lineas
   299-304): mutex, rwlock, variable de condicion e hilos. Si PSL1GHT no
   los trae, hay que escribir una capa de traduccion a lv2:

     pthread_mutex_t   -> sys_mutex_t      (sysMutexCreate/Lock/Unlock)
     pthread_cond_t    -> sys_cond_t       (sysCondCreate/Wait/Signal)
     pthread_rwlock_t  -> un mutex a secas (SCTP no depende de que dos
                          lectores entren a la vez; perder eso cuesta
                          rendimiento, no correccion)
     pthread_t         -> sys_ppu_thread_t (sysThreadCreate/Join)

   Es una cabecera de sustitucion y unos cuantos envoltorios, no un
   rediseño. Pero es trabajo, y mejor saberlo ahora.

   Pega esta salida entera y seguimos por ahi.
FIN
	exit 1
fi

# --- LOS TIPOS BSD, GENERADOS Y NO ADIVINADOS --------------------------
#
# Las cabeceras que vienen de FreeBSD usan los nombres BSD de toda la vida:
# u_int16_t, u_char, caddr_t. Newlib trae unos si y otros no, y cuales
# exactamente depende de como se compilo -- no es una lista que se pueda
# saber de memoria.
#
# Asi que se prueban uno a uno y se escribe una cabecera con SOLO los que
# falten. Definirlos todos a lo bruto chocaria con los que si estan
# (redefinir un typedef es error en C99), y definir de menos deja el mismo
# fallo que teniamos.
#
# Esto salio de que in_systm.h de FreeBSD usa u_int16_t y ppu-gcc no lo
# conoce: 22 ficheros con el mismo error. Tercera vez que una cabecera
# prestada trae una dependencia que la consola no tiene, asi que esta vez
# se resuelve la CLASE de problema y no el caso.

TIPOS="$WORK/.bsdtypes"
mkdir -p "$TIPOS"
BSDH="$TIPOS/gr33n_bsdtypes.h"

{
	echo "/* GENERADO por build-usrsctp.sh. No editar: se rehace cada vez."
	echo " * Contiene SOLO los tipos BSD que esta newlib no trae. */"
	echo "#ifndef GR33N_BSDTYPES_H"
	echo "#define GR33N_BSDTYPES_H"
	echo "#include <stdint.h>"
	echo "#include <sys/types.h>"
} > "$BSDH"

echo ">> tipos BSD que hay que poner"
n_tipos=0

tipo_bsd() {
	printf '#include <sys/types.h>\n#include <stdint.h>\n%s x;\nint main(void){(void)x;return 0;}\n' \
		"$1" > "$PROBE/t.c"

	if $CC -c "$PROBE/t.c" -o "$PROBE/t.o" -std=gnu99 $PEDANTE $PSL_INC > /dev/null 2>&1; then
		return 0
	fi

	echo "typedef $2 $1;" >> "$BSDH"
	echo "   falta $1, se define como $2"
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

# n_short, n_long y n_time NO van aqui: los define netinet/in_systm.h, que
# es su sitio de siempre, y ponerlos en los dos lados es un typedef
# duplicado, que en C99 es error y no aviso.

# --- Y BYTE_ORDER, QUE ES EL PEOR DE TODOS -----------------------------
#
# netinet/ip.h de FreeBSD declara struct ip DOS VECES, una por orden de
# bytes:
#
#     #if BYTE_ORDER == LITTLE_ENDIAN
#             u_char ip_hl:4, ip_v:4;
#     #endif
#     #if BYTE_ORDER == BIG_ENDIAN
#             u_char ip_v:4, ip_hl:4;
#     #endif
#
# Si BYTE_ORDER no esta definido, el preprocesador de C trata los
# identificadores desconocidos como CERO. O sea que las dos comparaciones
# dan 0 == 0, verdadero, y se compilan LAS DOS RAMAS: "duplicate member
# ip_v", 22 veces.
#
# Y ojo con lo que esto significa de verdad. Aqui el fallo fue ruidoso
# porque los miembros se repiten. Pero el mismo mecanismo, en una cabecera
# donde las dos ramas no chocaran, elegiria la de little-endian EN SILENCIO
# sobre una maquina big-endian. Ese es exactamente el fallo que llevamos
# toda la semana persiguiendo, y aqui lo produce una macro que falta.
#
# Se pregunta si la consola lo trae bien puesto; si no, se pone aqui, en la
# cabecera que entra con -include antes que ninguna otra.

# LA PRIMERA VERSION DE ESTA SONDA PREGUNTO LO QUE NO ERA.
#
# Incluia <machine/endian.h> y comprobaba que de ahi saliera BYTE_ORDER
# bien puesto. Salia, y dijo "la consola lo trae bien puesto" -- y luego
# fallaron los 22 ficheros igual, porque usrsctp NO incluye esa cabecera en
# ningun momento. La pregunta buena no era "se puede conseguir BYTE_ORDER"
# sino "esta BYTE_ORDER cuando ip.h se lee".
#
# Se prueba una cabecera detras de otra y se usa LA DE LA CONSOLA, con sus
# valores, en vez de inventarse unos propios que podrian no coincidir.

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
	echo "   BYTE_ORDER: sale de <$END_INC>, y se fuerza en todos los ficheros"
	{
		echo ""
		echo "/* De la propia consola, con SUS valores. Sin esto,"
		echo " * netinet/ip.h compila sus dos ramas de orden de bytes a la"
		echo " * vez: en C un identificador desconocido dentro de un #if"
		echo " * vale 0, y 0 == 0 es verdad las dos veces. */"
		echo "#include <$END_INC>"
	} >> "$BSDH"
else
	echo "   BYTE_ORDER: no lo da ninguna cabecera; se pone a mano"
	{
		echo ""
		echo "/* Ninguna cabecera de la consola lo da, asi que va a mano."
		echo " * Sin esto, netinet/ip.h compila sus dos ramas a la vez. */"
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
	echo "   ninguno: la consola los trae todos"
fi

# --- DE DONDE SALE sysGetRandomNumber ----------------------------------
#
# ESTE BLOQUE EXISTE PORQUE ME LO INVENTE.
#
# El parche de user_environment.c ponia `#include <lv2/random.h>` porque
# sonaba a lo que deberia llamarse. No existe. Los 23 ficheros no se
# enteraron -solo ese-, pero es exactamente la misma clase de fallo que
# llevamos toda la semana: la respuesta plausible usada sin comprobar.
#
# Lo que se sabe de verdad: source/net_tls.c de GR33N llama a
# sysGetRandomNumber y compila desde hace semanas. Pero incluye SIETE
# cabeceras de PSL1GHT y no hay forma de saber desde aqui cual de ellas la
# declara -las cabeceras estan en tu WSL, no en la mia-. Asi que se prueban
# una a una, con -Werror=implicit-function-declaration para que "compila con
# un aviso" no cuente como exito.
#
# Si ninguna cuela, se deja el prototipo a mano y se avisa por pantalla: hay
# que verificarlo contra la cabecera de verdad antes de fiarse, porque una
# firma equivocada aqui no da error, da entropia rota. Y una fuente de
# entropia rota no se nota: da claves predecibles.

RNDH="$TIPOS/gr33n_ps3_random.h"
RND_CAB=""

echo
echo ">> de donde sale sysGetRandomNumber"

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
	echo "/* GENERADO por build-usrsctp.sh. No editar: se rehace cada vez."
	echo " * Lo incluye el parche de user_environment.c. */"
	echo "#ifndef GR33N_PS3_RANDOM_H"
	echo "#define GR33N_PS3_RANDOM_H"
} > "$RNDH"

if [ -n "$RND_CAB" ]; then
	echo "   la declara <$RND_CAB>"
	echo "#include <$RND_CAB>" >> "$RNDH"
else
	echo "   !! NINGUNA de las candidatas la declara."
	echo "      Se pone el prototipo a mano, PERO HAY QUE COMPROBARLO:"
	echo "      busca la buena con"
	echo "        grep -rl sysGetRandomNumber \$PS3DEV/ppu/include"
	echo "      y pegame la linea de la declaracion."
	{
		echo "/* NO SE ENCONTRO LA CABECERA. Prototipo deducido de la"
		echo " * llamada de source/net_tls.c:245, que compila:"
		echo " *     ret = sysGetRandomNumber(tmp, (u64)ask);"
		echo " * Si la firma de verdad no es esta, el enlazado cuela"
		echo " * igual -en C no hay decoracion de nombres- y lo que"
		echo " * sale roto es la entropia, en silencio. */"
		echo "int sysGetRandomNumber(void *addr, unsigned long long size);"
	} >> "$RNDH"
fi

echo "#endif" >> "$RNDH"

echo
echo ">> la consola trae todo lo que usrsctp pide. Adelante."
echo

# --- las cabeceras que a PSL1GHT le faltan ------------------------------
#
# deps/sonda-cabeceras.sh pregunto por 43 y PSL1GHT trae 27. Faltan 16, y
# ademas su sys/queue.h existe SIN NINGUNA macro TAILQ, que usrsctp usa por
# todas partes -- 68 macros distintas de LIST, SLIST, STAILQ y TAILQ.
#
# Las de formato de cable (netinet/ip.h, udp.h) y sys/queue.h vienen de
# FreeBSD tal cual, con su licencia. Escribir de memoria una struct de
# cable con campos de bits, en la plataforma donde el orden de bytes ya nos
# ha mordido esta semana, seria tentar a la suerte.
#
# OJO A LA VUELTA DE TUERCA: el TIPO struct iovec si existe en PSL1GHT -- lo
# define algo que incluye su sys/socket.h -- pero el FICHERO no. Asi que el
# sustituto no puede definir el struct a ciegas (sale "redefinition") ni
# puede quedarse vacio (falta UIO_MAXIOV, que usrsctp usa en
# user_socket.c:585). Lo resuelve la sonda de arriba, que pregunta en vez
# de suponer. Ver el comentario largo de ps3-shim/sys/uio.h.
#
# De readv y writev no hay que preocuparse: en todo usrsctplib no hay una
# sola llamada a ninguna de las dos.
#
# El directorio va DELANTE de las cabeceras de PSL1GHT en la ruta, pero
# solo contiene uio.h: todo lo demas lo sigue resolviendo la consola.

SHIM="$HERE/ps3-shim"

for f in sys/uio.h sys/queue.h sys/socket.h sys/time.h net/if.h ifaddrs.h \
         netinet/ip.h netinet/udp.h netinet/in_systm.h netinet/in.h \
         endian.h errno.h ps3_stubs.c; do
	if [ ! -f "$SHIM/$f" ]; then
		echo "falta $SHIM/$f"
		echo "el arbol de sustitutos tiene que estar en deps/ps3-shim/"
		exit 1
	fi
done

echo ">> cabeceras de sustitucion en $SHIM"
echo "   (16 que PSL1GHT no trae, mas sys/queue.h porque la suya no tiene"
echo "    ni una macro TAILQ. Ver $SHIM/LEEME.md)"

# --- fuentes ------------------------------------------------------------

if [ ! -d "$SRC" ]; then
	echo ">> clonando usrsctp"
	git clone --depth 1 https://github.com/sctplab/usrsctp.git "$SRC"
else
	echo ">> usando $SRC (ya clonado)"
fi

# --- el parche ----------------------------------------------------------
#
# Tres cambios, y el primero es el que evita una tarde perdida:
#
#   1. sockaddr_conn con el byte de longitud delante. La newlib de PSL1GHT
#      viene de BSD, asi que struct sockaddr lleva sa_len y la familia va
#      en el desplazamiento 1. Sin esto, sconn_family se lee mal y se
#      corrompe el monton al crear el socket. Diagnostico de green-nx, que
#      se lo comio en Switch con la misma familia de newlib.
#   2. read_random sobre sysGetRandomNumber, con la comprobacion de que no
#      devuelva solo ceros.
#   3. La misma definicion en la cabecera publica y en la interna.

cd "$SRC"

# PRIMERO SE DESHACE LO DE LA VUELTA ANTERIOR.
#
# $SRC es un clon de upstream y NADIE lo edita a mano: lo unico que cambia
# ahi dentro es este parche. Asi que devolverlo a como vino no pierde nada.
#
# Sin esto, cada vez que el parche cambia el script se planta: el de antes
# ya esta aplicado, el nuevo no encaja sobre el, y `git apply --reverse`
# tampoco, porque no son el mismo parche. Salia "el parche no aplica sobre
# esta version de usrsctp / probablemente upstream ha cambiado", que es un
# diagnostico falso y de los que mandan a mirar donde no es.
if git rev-parse --git-dir > /dev/null 2>&1; then
	git checkout -- . 2>/dev/null || true
	git clean -fd > /dev/null 2>&1 || true
fi

if git apply --check "$HERE/usrsctp-ps3.patch" 2>/dev/null; then
	git apply "$HERE/usrsctp-ps3.patch"
	echo ">> parche de PS3 aplicado"
elif git apply --reverse --check "$HERE/usrsctp-ps3.patch" 2>/dev/null; then
	echo ">> el parche de PS3 ya estaba aplicado"
else
	echo "!! el parche no aplica sobre esta version de usrsctp."
	echo "   Probablemente upstream ha cambiado. Pega esto y lo rehago:"
	git apply --verbose "$HERE/usrsctp-ps3.patch" 2>&1 | head -20
	exit 1
fi
cd - > /dev/null

# --- compilacion --------------------------------------------------------

CFLAGS="-O2 -Wall -std=gnu99 -mcpu=cell"

# El sustituto PRIMERO, para que <sys/uio.h> lo encuentre; el resto de
# <sys/...> lo sigue poniendo PSL1GHT porque ahi dentro no hay nada mas.
CFLAGS="$CFLAGS -I$SHIM $IOVEC_DEF $IN6_DEF $SS_DEF $PKT_DEF"

# Los tipos BSD que falten, delante de todo. Ver el bloque de arriba.
CFLAGS="$CFLAGS -include $BSDH"

# Y el directorio de lo generado, para que el parche de user_environment.c
# encuentre gr33n_ps3_random.h.
CFLAGS="$CFLAGS -I$TIPOS"

CFLAGS="$CFLAGS -I$SRC/usrsctplib -I$SRC/usrsctplib/netinet $PSL_INC"

# LA OPCION QUE HABRIA AHORRADO LA VUELTA ANTERIOR.
#
# Sin esto, una macro que no existe -CMSG_SPACE, timercmp, timingsafe_bcmp-
# se convierte en una llamada a funcion implicita, que en C99 es un AVISO. El
# fichero compila, entra en la biblioteca, y el fallo aparece mas tarde y en
# otro sitio:
#
#   - si el simbolo no existe, en el enlazado, sin decir quien lo llamaba;
#   - si la macro devolvia un PUNTERO -CMSG_DATA, CMSG_NXTHDR-, no aparece
#     nunca: C99 supone que devuelve int, y en un binario de 32 bits ese int
#     tiene el mismo tamaño que el puntero. Compila, enlaza, arranca, y
#     escribe en una direccion truncada dentro de la consola.
#
# Ese ultimo caso es el que da miedo, y es exactamente el que teniamos:
# sctp_indata.c hace memcpy(CMSG_DATA(cmh), ...) en el camino de recepcion.
#
# OJO A LO QUE ESTO PUEDE HACERLE A LA CUENTA. Ficheros que la vuelta pasada
# "compilaron" pueden fallar ahora, porque antes pasaban con avisos. Eso NO
# es un retroceso: es lo mismo que ya estaba mal, dicho a tiempo.
CFLAGS="$CFLAGS -Werror=implicit-function-declaration -Werror=implicit-int"

# El sys/socket.h de PSL1GHT declara sysNetSelect con un struct timeval*
# antes de que struct timeval exista, y eso saca un aviso en CADA fichero.
# No es fallo nuestro y no rompe nada, pero veintidos copias del mismo
# aviso tapan los que si importan. Incluyendo sys/time.h antes, el tipo ya
# esta completo cuando se lee esa linea.
#
# SE LE PREGUNTA AL COMPILADOR, NO AL SISTEMA DE FICHEROS. La version
# anterior miraba si existia $PSL_DIR/sys/time.h y no lo encontraba nunca,
# porque esa cabecera vive en el sysroot de newlib y no en el directorio de
# PSL1GHT. El aviso siguio saliendo veintidos veces sin que nadie supiera
# por que. Mismo error que con BYTE_ORDER: preguntar donde no es.
printf '#include <sys/time.h>\nint main(void){struct timeval t;(void)t;return 0;}\n' \
	> "$PROBE/tv.c"
if $CC -c "$PROBE/tv.c" -o "$PROBE/tv.o" -std=gnu99 $PSL_INC > /dev/null 2>&1; then
	CFLAGS="$CFLAGS -include sys/time.h"
fi

# LA LINEA DE LA QUE DEPENDE QUE ESTO FUNCIONE.
#
# usrsctp declara sus cabeceras de cable por duplicado y elige con
# WORDS_BIGENDIAN, que NO autodetecta para esta plataforma: solo se deduce
# sola para __APPLE__ con PowerPC (sctp_os_userspace.h:278). Sin definirla
# compila sin un solo aviso con las estructuras de little-endian sobre una
# maquina big-endian, y todos los paquetes SCTP se leen mal. No hay error:
# hay input que no llega.
CFLAGS="$CFLAGS -DWORDS_BIGENDIAN=1"

# Y quien somos, para las tres ramas del parche.
CFLAGS="$CFLAGS -DGR33N_PS3=1"

# Lo que espera usrsctp de un anfitrion de espacio de usuario.
CFLAGS="$CFLAGS -D__Userspace__ -DSCTP_SIMPLE_ALLOCATOR -DSCTP_PROCESS_LEVEL_LOCKS"
CFLAGS="$CFLAGS -DHAVE_SA_LEN -DHAVE_SIN_LEN -DHAVE_SIN6_LEN -DHAVE_SCONN_LEN"

# SOLO IPv4, Y A PROPOSITO.
#
# Con -DINET6, user_recv_thread.c pide struct in6_pktinfo, que es de los
# sockets IPv6 en bruto y no existe aqui. Se podria pelear, pero no tiene
# sentido: la PS3 no habla IPv6, y la direccion IPv6 que da xCloud en
# /configuration es Teredo -- un tunel sobre IPv4 que tampoco podriamos
# usar. Declarar soporte de algo que no se puede hacer es la misma clase
# de mentira que el RESOLUTION=63 del PARAM.SFO que congelaba la consola.
#
# Comprobado con el cruzado: con INET6 fallan 1 de 23; sin el, 23 de 23.
CFLAGS="$CFLAGS -DINET"

# Aliasing estricto fuera, por lo mismo que en libSRTP: SCTP lee palabras
# de 32 bits desde buffers de char por todas partes.
CFLAGS="$CFLAGS -fno-strict-aliasing"

# usrsctp viene de FreeBSD y arrastra avisos que no son nuestros. Se
# silencian los de ruido, NUNCA los que hablan de tamaños o punteros.
CFLAGS="$CFLAGS -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function"

# --- LA SEGUNDA SONDA, ESTA VEZ CON LAS OPCIONES DE VERDAD -------------
#
# La sonda de arriba compila con $PSL_INC a secas. El build compila con el
# arbol de sustitutos DELANTE, y ahi <sys/socket.h> ya no es el de la
# consola sino la envoltura de ps3-shim. O sea que las dos preguntas no son
# la misma pregunta, y la de arriba no puede contestar por esta.
#
# Es la misma leccion de BYTE_ORDER: medir donde ocurre el problema. Aqui se
# mide en el sitio exacto, y ademas se dice QUIEN pone cada cosa, para que
# no haya que deducirlo de los errores la proxima vez.

quien_pone() {
	{
		printf '#include <%s>\n' "$2"
		printf '#ifdef %s\n#error lo pone el shim\n#endif\n' "$3"
		printf 'int main(void){return 0;}\n'
	} > "$PROBE/w.c"

	# shellcheck disable=SC2086
	if $CC -c "$PROBE/w.c" -o "$PROBE/w.o" $CFLAGS > /dev/null 2>&1; then
		echo "   $1: lo trae la consola"
	else
		echo "   $1: lo pone ps3-shim"
	fi
}

echo
echo ">> con las opciones del build, quien pone que"
quien_pone "CMSG_DATA/SPACE/LEN" sys/socket.h GR33N_CMSG_PUESTAS_AQUI
quien_pone "CMSG_ALIGN         " sys/socket.h GR33N_CMSG_ALIGN_PUESTA_AQUI
quien_pone "timercmp/add/sub   " sys/time.h   GR33N_TIMERCMP_PUESTA_AQUI
echo

# ps3_stubs.c entra en la biblioteca: son las cuatro funciones de enumerar
# interfaces que usrsctp llama y la consola no tiene. Devuelven "no hay
# nada", que con AF_CONN es la verdad y no un apaño. Ver LEEME.md.
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

# user_recv_thread.c NO ESTA EN LA LISTA, Y ES A PROPOSITO.
#
# Es el modo en el que usrsctp abre sus propios sockets -uno crudo de SCTP y
# otro UDP para el tunel- con hilos dedicados leyendo de ellos. 1500 lineas
# que llaman a socket(), bind(), setsockopt(), recvmsg() y close(), y PSL1GHT
# no tiene ninguna con esos nombres: su API es netSocket/netBind/netRecv.
# Eran cinco simbolos sin resolver en el enlazado del EBOOT por codigo que no
# se ejecuta nunca, porque libpeer arranca usrsctp con AF_CONN.
#
# Las DOS unicas funciones que ese fichero exporta -recv_thread_init y
# recv_thread_destroy, comprobado con nm- estan vacias en ps3_stubs.c, que es
# lo mismo que hace WebRTC con usrsctp_init_nothreads(). El comentario largo
# esta alli.

OBJ="$WORK/.obj-usrsctp"
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT/lib" "$OUT/include"

echo ">> compilando con $(basename "$CC")"

n=0
fail=0
for f in $FILES; do
	b=$(basename "$f" .c)

	# ps3_stubs.c viene con ruta absoluta; el resto cuelga de $SRC.
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
		# ENTEROS Y SOLO LOS ERRORES.
		#
		# Antes esto era `head -10`, y en sctp_pcb.c se comio el
		# tercer error -una asignacion a un pthread_t que es struct-
		# porque los dos primeros ocupaban las diez lineas con sus
		# cursores. Una vuelta entera perdida por ahorrar salida.
		#
		# Se quitan los avisos y las lineas de contexto y se deja el
		# mensaje de cada error, que es lo unico que hace falta leer.
		grep -E "error:|Error [0-9]" "$OBJ/$b.err" | sed 's/^/     /'
	fi
done

echo ">> $n compilados, $fail fallidos"

if [ "$fail" -ne 0 ]; then
	echo
	echo "   Los ficheros de error completos -con avisos y contexto- estan en"
	echo "   $OBJ/*.err  por si hace falta mirar alguno entero."
	echo "   Pega esta salida y seguimos."
	echo
	exit 1
fi

"$AR" rcs "$OUT/lib/libusrsctp.a" "$OBJ"/*.o
cp "$SRC/usrsctplib/usrsctp.h" "$OUT/include/"

# --- que queda sin resolver ---------------------------------------------

if [ -x "$NM" ]; then
	"$NM" --undefined-only "$OUT/lib/libusrsctp.a" 2>/dev/null \
		| awk '{print $2}' | grep -v '^$' | sort -u > "$WORK/.undef"
	"$NM" --defined-only "$OUT/lib/libusrsctp.a" 2>/dev/null \
		| awk '{print $3}' | grep -v '^$' | sort -u > "$WORK/.def"

	comm -23 "$WORK/.undef" "$WORK/.def" > "$WORK/.falta"

	echo
	echo ">> simbolos sin resolver:"
	sed 's/^/   /' "$WORK/.falta"

	echo
	echo "   Esperado: libc de newlib (memcpy, malloc, printf, __errno...),"
	echo "   pthread_*, usleep, gettimeofday, y sysGetRandomNumber de lv2."

	# ESTO NO SE MIRA A OJO: SE COMPRUEBA.
	#
	# usrsctp habla por AF_CONN, o sea que NO abre sockets: los paquetes se
	# los damos con usrsctp_conninput() y nos los devuelve por la
	# devolucion de llamada. Quien los pone en el cable es libpeer, por
	# DTLS, sobre el socket UDP que ya maneja GR33N.
	#
	# Asi que ninguno de estos nombres deberia quedar pendiente. Y ademas
	# PSL1GHT no los tiene: su API es netSocket/netBind/netSetSockOpt/
	# netRecv/netSendTo/netClose. Si aparecen, el EBOOT no enlaza, y el
	# error saldria mucho mas tarde y hablando de otra cosa.
	#
	# La vuelta pasada esto era un parrafo pidiendole al lector que mirase.
	# Miro, y estaban los seis. Un aviso que hay que leer no es una
	# comprobacion.
	RED=$(grep -x -E 'socket|bind|listen|accept|connect|sendmsg|recvmsg|sendto|recvfrom|setsockopt|getsockopt|ioctl|select|poll' \
	      "$WORK/.falta" || true)

	if [ -n "$RED" ]; then
		echo
		echo "!! HAY SIMBOLOS DE RED SIN RESOLVER:"
		printf '%s\n' "$RED" | sed 's/^/     /'
		echo
		echo "   Con AF_CONN usrsctp no deberia abrir un socket jamas, y"
		echo "   PSL1GHT no tiene estos nombres. Tal y como esta, el EBOOT"
		echo "   no va a enlazar."
		echo
		echo "   Para saber QUIEN los pide:"
		echo "     $NM --undefined-only $OUT/lib/libusrsctp.a | less"
		echo "   (el nombre del .o sale en la linea de arriba de cada bloque)"
		echo
		echo "   Pega eso y lo miramos."
	else
		echo
		echo "   Y ninguno de red, que es lo que tiene que salir: comprobado"
		echo "   contra la lista de socket/bind/sendmsg/ioctl/setsockopt..."
	fi

	rm -f "$WORK/.undef" "$WORK/.def" "$WORK/.falta"
fi

rm -rf "$OBJ"

echo
echo ">> listo: $OUT/lib/libusrsctp.a  ($(du -h "$OUT/lib/libusrsctp.a" | cut -f1))"
