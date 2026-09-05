#!/bin/sh
#
# GR33N - construye libopus para PS3 (PSL1GHT / ppu-gcc)
#
#   sh build-opus.sh
#
# Deja $HOME/.gr33n-deps/opus-ps3/ con:
#   include/opus/       cabeceras
#   lib/libopus.a
#
# Misma forma que build-mbedtls.sh y build-libsrtp.sh, y por las mismas
# razones: todo dentro de $HOME y nunca en /mnt/c (DrvFs no deja cambiar
# permisos), y una salida provisional que solo se mueve a su sitio cuando
# todas las comprobaciones pasan -- que eso ya mordio una vez, cuando un
# build-libpeer.sh fallido borro peer.h y el make siguiente murio con un
# error que no tenia nada que ver.
#
# ------------------------------------------------------------------
# POR QUE OPUS Y NO OTRA COSA
# ------------------------------------------------------------------
#
# Porque es lo que manda xCloud y no hay eleccion. La oferta SDP que ya
# sale de la consola dice:
#
#   m=audio 9 UDP/TLS/RTP/SAVPF 111
#   a=rtpmap:111 opus/48000/2
#
# O sea que llevamos semanas recibiendo audio Opus y tirandolo por no
# tener con que descodificarlo.
#
# ------------------------------------------------------------------
# PUNTO FIJO, Y NO ES UNA PRECAUCION TONTA
# ------------------------------------------------------------------
#
# --enable-fixed-point. El PPE del Cell es un nucleo en orden de 2006 con
# una unidad de coma flotante que no destaca, y ademas ya lleva encima el
# bombeo de WebRTC, el descifrado SRTP y el reparto al RSX. El SPU esta
# ocupado con H.264.
#
# La version de punto fijo de Opus es la que usan los telefonos: no es un
# camino raro ni poco probado, es el mas probado de los dos. La diferencia
# de calidad frente al flotante es inaudible -- lo dice la propia gente de
# Opus-- y el consumo es mucho mas predecible, que en una consola donde ya
# vamos justos importa mas que el ultimo decibelio.
#
# Si algun dia sobra CPU, se quita el --enable-fixed-point y se mide. Pero
# se mide: no se cambia por corazonada.
#
# ------------------------------------------------------------------
# NO HACE FALTA REMUESTREAR, Y ESO ES UN REGALO
# ------------------------------------------------------------------
#
# Opus descodifica a 48000 Hz. El puerto de audio de la PS3 sale a 48000
# Hz. Son el mismo numero, asi que no hay remuestreo, no hay filtro, no hay
# error acumulado y no hay una tercera biblioteca que compilar.
#
# Es de las pocas cosas de este puerto que salen gratis. Conviene no
# estropearlo metiendo un remuestreador "por si acaso".

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
	echo "no encuentro $CC"
	echo "exporta PS3DEV o corrige la ruta"
	exit 1
fi

# --- fuentes ----------------------------------------------------------

# EL COMMIT CLAVADO, por lo mismo que en libsrtp: master no es una version,
# es lo que haya hoy.
#
# v1.4, no "v1.4.3": esa version NO EXISTE, me la invente. Las de xiph/opus
# son v1.3.1, v1.4, v1.5, v1.5.1, v1.5.2. Ahora el script lo comprueba en
# vez de fiarse, y si el tag no esta te ensena los que si.
#
# La 1.4 y no la 1.5 porque la 1.5 trae DRED, un modelo de red neuronal
# para esconder perdidas. En un PPE de 2006 eso no pinta nada.
OPUS_REF=v1.4

if [ ! -d "$SRC" ]; then
	echo ">> clonando opus"
	git clone --quiet https://github.com/xiph/opus.git "$SRC"
fi

echo ">> opus en $OPUS_REF"

# EL PARENTESIS QUE SE TRAGABA EL FALLO.
#
# Aqui habia un subshell que terminaba en `git checkout -- . || true`. El
# estado de salida de un subshell es el de su ULTIMO comando, y ese ultimo
# comando era literalmente incapaz de fallar. Asi que el `|| exit 1` de
# fuera no se disparaba nunca.
#
# Resultado: "error: pathspec 'v1.4.3' did not match any file(s)" salio por
# pantalla, el script dijo ">> reuniendo fuentes" y siguio tan tranquilo
# compilando MASTER. Los errores de qext_cache y NB_QEXT_BANDS de despues
# eran eso: codigo de la 1.5 que no deberia haber estado ahi.
#
# Una guarda que no puede fallar no es una guarda. Van tres en este
# proyecto, y esta la escribi yo mismo hace media hora.
if ! (
	cd "$SRC" &&
	{ git checkout --quiet "$OPUS_REF" 2>/dev/null ||
	  { git fetch --quiet --tags origin &&
	    git checkout --quiet "$OPUS_REF"; }; } &&
	git checkout --quiet -- .
); then
	echo "!! no existe el tag $OPUS_REF en este clon."
	echo
	echo "   Los que si hay:"
	(cd "$SRC" && git tag -l 'v*' | tail -12 | sed 's/^/     /')
	echo
	echo "   Si el clon era --depth 1, borralo y repite:  rm -rf $SRC"
	exit 1
fi

# --- y AHORA se comprueba de verdad que version es ---------------------
#
# La comprobacion que habia aqui buscaba opus_decode_float en opus.h. Esa
# funcion existe desde la 1.0: la comprobacion pasaba con CUALQUIER
# version, incluida la master que no queriamos. Comprobar algo que siempre
# es cierto es no comprobar nada.
#
# Estas dos si distinguen:

VER=$( cd "$SRC" && git describe --tags --exact-match HEAD 2>/dev/null || echo "(ninguno)" )
if [ "$VER" != "$OPUS_REF" ]; then
	echo "!! HEAD esta en '$VER', no en '$OPUS_REF'."
	exit 1
fi
echo "   git describe dice: $VER"

# NB_QEXT_BANDS solo existe a partir de la 1.5. Si aparece, esto es master
# por mucho que el tag diga otra cosa.
if grep -rq "NB_QEXT_BANDS" "$SRC/celt/" 2>/dev/null; then
	echo "!! este arbol tiene QEXT: es 1.5 o master, no la 1.4."
	echo "   Limpia y repite:  rm -rf $SRC"
	exit 1
fi
echo "   sin QEXT: es la rama 1.4"

# --- configuracion a mano ---------------------------------------------
#
# NADA DE ./configure. Opus trae autotools y cmake, y los dos quieren
# compilar y EJECUTAR programas de prueba para averiguar cosas del sistema.
# Cruzando a PowerPC eso no se puede: el binario no corre aqui. Se puede
# pelear con --host y un cache de respuestas, o se puede escribir el
# config.h a mano, que son veinte lineas y no miente.
#
# Es exactamente lo que ya se hizo con mbedTLS y con libSRTP, y por lo
# mismo.

mkdir -p "$WORK/.opus-cfg"
cat > "$WORK/.opus-cfg/config.h" <<'CFG'
/* GR33N - config.h de opus para PS3, escrito a mano.
 *
 * ------------------------------------------------------------------
 * AQUI CASI ME LA PEGO, Y EL FALLO HABRIA SIDO MUDO
 * ------------------------------------------------------------------
 *
 * La primera version de este fichero ponia cosas como:
 *
 *     #define DISABLE_FLOAT_API  0
 *     #define HAVE_LRINTF        0
 *     #define USE_ALLOCA         0
 *
 * leyendo "0" como "apagado". Pero opus NO las prueba con #if, las prueba
 * con #ifdef:
 *
 *     #ifndef DISABLE_FLOAT_API      <- en opus.h, alrededor de
 *     ...                               opus_decode_float
 *     #endif
 *
 * Definir algo a 0 lo deja DEFINIDO. O sea que esas tres lineas
 * significaban exactamente lo contrario de lo que parecian:
 *
 *   - DISABLE_FLOAT_API 0  ->  se va opus_decode_float, que es justo la
 *                              funcion que aud.c necesita. La comprobacion
 *                              del final del script la habria cazado, pero
 *                              con "no esta" y sin decir por que.
 *   - HAVE_LRINTF 0        ->  opus llama a lrintf, que newlib puede no
 *                              traer. Error de enlace al final de todo.
 *   - USE_ALLOCA 0         ->  alloca() en hilos con pila de 64 KB.
 *                              Eso no da error: da pisotones de memoria.
 *
 * La regla, para no repetirlo: aqui NO se apaga nada poniendolo a cero.
 * Lo que no se quiere, NO SE ESCRIBE.
 */
#ifndef GR33N_OPUS_CONFIG_H
#define GR33N_OPUS_CONFIG_H

#define OPUS_BUILD       1
#define PACKAGE_VERSION  "1.4-gr33n"

/* PUNTO FIJO. Ver el comentario largo de build-opus.sh. */
#define FIXED_POINT      1

/* Arrays de tamano variable de C99 en vez de alloca(). Los hilos de este
 * proyecto tienen pilas medidas y alloca no respeta ninguna. */
#define VAR_ARRAYS       1

/* Y NADA MAS.
 *
 * En particular NO estan, a proposito y no por olvido:
 *   DISABLE_FLOAT_API   - se quiere el API flotante (el puerto de la PS3
 *                         come float; convertir aqui ahorra un paso)
 *   HAVE_LRINTF / HAVE_LRINT - newlib no los garantiza
 *   USE_ALLOCA          - ver arriba
 *   OPUS_HAVE_RTCD      - el PPE tiene VMX, pero el camino PowerPC de opus
 *                         no esta escrito ni probado para un nucleo en
 *                         orden. El C de siempre es correcto, y aqui la
 *                         correccion vale mas. Si algun dia sobra CPU,
 *                         esto es lo primero que se mide.
 */

#endif
CFG

# --- que fuentes ------------------------------------------------------
#
# SE LAS PREGUNTAMOS A OPUS. No se listan a mano y tampoco se buscan con
# find: opus trae sus propias listas en celt_sources.mk, silk_sources.mk y
# opus_sources.mk, que es lo que usa su Makefile.am de verdad.
#
# El intento anterior era un find con exclusiones (-not -path '*/arm/*',
# '*/x86/*'...) y se dejo fuera celt/dump_modes/, que es una herramienta de
# construccion y no biblioteca. De ahi los errores de dump_modes.c.
#
# Una lista de exclusiones es una lista paralela mantenida a mano por la
# puerta de atras: hay que acordarse de cada directorio nuevo que aparezca.
# Las .mk son la lista de verdad, la mantiene opus, y separan ademas el
# punto fijo del flotante sin que tengamos que decidirlo nosotros.
#
# Se leen con make, que es quien sabe leerlas. Escribir un parser de
# Makefiles a mano para esto seria la tercera lista.

echo ">> preguntandole a opus que ficheros son suyos"

cat > "$WORK/.opus-cfg/listar.mk" <<'MK'
include celt_sources.mk
include silk_sources.mk
include opus_sources.mk

listar:
	@echo $(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FIXED) \
	      $(OPUS_SOURCES) $(OPUS_SOURCES_FLOAT)
MK

FUENTES_REL=$(make -s -C "$SRC" -f "$WORK/.opus-cfg/listar.mk" listar) || {
	echo "!! no he podido leer las listas .mk de opus."
	echo "   Mira si estan:  ls $SRC/*_sources.mk"
	exit 1
}

# A rutas absolutas, y comprobando que cada una existe. Un fichero que la
# .mk nombra y no esta es una senal de arbol incompleto, y vale mas verlo
# aqui que en forma de simbolo sin resolver dentro de media hora.
FUENTES=""
N=0
for f in $FUENTES_REL; do
	case "$f" in
		*.c) ;;
		*) continue ;;
	esac

	if [ ! -f "$SRC/$f" ]; then
		echo "!! la lista nombra $f y no existe en el arbol"
		exit 1
	fi

	FUENTES="$FUENTES $SRC/$f"
	N=$((N + 1))
done

echo "   $N ficheros, de las listas de la propia opus"

# NO SE COMPRUEBA CONTRA UN NUMERO, y esto ya me mordio: la primera version
# exigia "al menos 150 ficheros" -- un umbral que me saque de la manga
# comparandolo con los 137 que habia juntado el find roto de antes. Las
# listas de la 1.4 dan 131, que es LO CORRECTO, y la comprobacion tumbo un
# resultado bueno.
#
# Un umbral inventado no comprueba nada: solo dice si el numero se parece
# al que uno esperaba. Lo que de verdad importa es que las CUATRO listas
# hayan aportado algo; si una variable no se expande, su grupo entero
# desaparece y eso si es un fallo de verdad. Se mira por prefijo, que no
# depende de que yo recuerde bien ningun nombre de fichero.

falta_grupo=0
for pre in "celt/" "silk/" "silk/fixed/" "src/"; do
	if ! echo "$FUENTES_REL" | tr " " "\n" | grep -q "^$pre"; then
		echo "!! ningun fichero de $pre: esa lista no se ha expandido"
		falta_grupo=1
	fi
done

# Y dos ficheros concretos, comprobados uno a uno contra el arbol de la
# v1.4 antes de escribirlos aqui. Si estos dos estan, las listas se han
# leido.
for f in silk/decode_core.c silk/fixed/find_LPC_FIX.c; do
	if ! echo "$FUENTES_REL" | tr " " "\n" | grep -qx "$f"; then
		echo "!! falta $f en la lista"
		falta_grupo=1
	fi
done

if [ "$falta_grupo" = "1" ]; then
	echo
	echo "   Pruebalo a mano:"
	echo "     make -s -C $SRC -f $WORK/.opus-cfg/listar.mk listar"
	exit 1
fi

echo "   las cuatro listas han aportado ficheros"

# --- las cabeceras de PSL1GHT -----------------------------------------
#
# Igual que en libsrtp: no estan en la ruta por defecto de ppu-gcc, las
# pone el Makefile del proyecto desde ppu_rules. Compilando a mano no las
# pone nadie. Opus casi no usa cabeceras del sistema, pero <stdint.h> y
# compania si, y mas vale tener la ruta buena.

PSL_INC=""
for d in "$PS3DEV/ppu/include" "$PSL1GHT/ppu/include" \
         "$PS3DEV/portlibs/ppu/include"; do
	if [ -f "$d/netinet/in.h" ]; then
		PSL_INC="-I$d"
		echo ">> cabeceras de PSL1GHT en $d"
		break
	fi
done

[ -n "$PSL_INC" ] || echo ">> sin cabeceras de PSL1GHT (opus casi no las usa)"

# --- compilacion ------------------------------------------------------

CFLAGS="-O2 -std=gnu99 -mcpu=cell -DHAVE_CONFIG_H"
CFLAGS="$CFLAGS -I$WORK/.opus-cfg"
CFLAGS="$CFLAGS -I$SRC/include -I$SRC/celt -I$SRC/silk -I$SRC/silk/fixed"
CFLAGS="$CFLAGS -I$SRC/src"
CFLAGS="$CFLAGS $PSL_INC"

# Aliasing estricto FUERA, por lo mismo que en libSRTP: Opus lee palabras
# desde buffers de char en el empaquetador de rangos. En x86 sale bien de
# casualidad.
CFLAGS="$CFLAGS -fno-strict-aliasing"

rm -rf "$OBRAS"
mkdir -p "$OBRAS/obj" "$OBRAS/lib" "$OBRAS/include/opus"

echo ">> compilando ($N ficheros, esto tarda un rato)"

FALLOS=0
i=0
for f in $FUENTES; do
	i=$((i + 1))
	o="$OBRAS/obj/$(echo "$f" | sed "s|$SRC/||; s|/|_|g; s|\.c$|.o|")"

	if ! $CC $CFLAGS -c "$f" -o "$o" 2> "$OBRAS/err.txt"; then
		echo
		echo "!! falla: $f"
		sed 's/^/   /' "$OBRAS/err.txt" | head -20
		FALLOS=$((FALLOS + 1))
		[ "$FALLOS" -ge 3 ] && {
			echo
			echo "   tres fallos, paro. Los de arriba suelen ser el mismo"
			echo "   problema repetido."
			exit 1
		}
	fi

	[ $((i % 40)) -eq 0 ] && echo "   $i/$N"
done

[ "$FALLOS" -eq 0 ] || { echo "!! $FALLOS ficheros no compilan"; exit 1; }

echo "   $N/$N"

# --- biblioteca -------------------------------------------------------

echo ">> archivando"
$AR rcs "$OBRAS/lib/libopus.a" "$OBRAS"/obj/*.o
$RANLIB "$OBRAS/lib/libopus.a" 2>/dev/null || true

cp "$SRC/include/"*.h "$OBRAS/include/opus/"

# --- las comprobaciones -----------------------------------------------
#
# Que compile no quiere decir que sirva. Lo que hace falta es que estén
# las funciones que aud.c va a llamar, y que no falte nada del sistema.

echo
echo "=============================================================="
echo " comprobaciones"
echo "=============================================================="

# UNA FUNCION EN POWERPC64 TIENE DOS SIMBOLOS, Y ESO TUMBO LAS NUEVE.
#
# El PPU usa el ABI ELFv1, donde una funcion no es una direccion: es un
# DESCRIPTOR de tres palabras que vive en la seccion .opd y apunta al
# codigo. Asi que nm ensena dos entradas por funcion:
#
#     0000000000000000 D opus_decoder_create      <- el descriptor
#     0000000000000000 T .opus_decoder_create     <- el codigo, con punto
#
# La comprobacion de antes exigia " T opus_decoder_create" y daba NO en
# las nueve, con libopus.a perfectamente construida. Un "no esta" que en
# realidad decia "no esta como yo esperaba".
#
# Lo curioso es que la sonda de audio SI lo hacia bien --filtraba por
# " T | D | B "-- y lo tenia delante en su salida: audioInit salia junto a
# __audioInit. Estaba escrito y no lo mire.
#
# Ahora se compara el NOMBRE, con o sin punto delante, y si falla se
# ensena lo que nm dice de verdad en vez de dejarlo en un NO a secas.
falta=0
for s in opus_decoder_create opus_decoder_destroy opus_decode \
         opus_decode_float opus_decoder_ctl opus_packet_get_nb_channels \
         opus_packet_get_nb_frames opus_packet_get_samples_per_frame \
         opus_strerror; do
	if $NM --defined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
	   awk '{print $NF}' | grep -Eqx "\.?$s"; then
		echo "   $s ... SI"
	else
		echo "   $s ... NO  <<<<"
		echo "      lo que nm dice de ese nombre:"
		$NM --defined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
			grep -- "$s" | head -4 | sed 's/^/        /'
		falta=1
	fi
done

if [ "$falta" = "1" ]; then
	echo
	echo "!! faltan funciones que aud.c necesita. NO se instala."
	echo "   La biblioteca a medias se queda en $OBRAS para mirarla."
	exit 1
fi

echo
echo " simbolos SIN RESOLVER que no sean de libc:"
echo " (si aqui sale algo raro, opus espera algo del sistema que la"
echo "  consola no tiene, y es mejor verlo ahora que entre cien"
echo "  errores de enlace que no dicen nada)"
# $NF y no $2, y quitando el punto de delante: por lo mismo de arriba, en
# ELFv1 un simbolo de funcion puede venir como ".foo". Y fuera las lineas
# de cabecera de miembro del archivo, que acaban en ":".
$NM --undefined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
	awk '{ n = $NF; sub(/^\./, "", n); print n }' | sort -u |
	grep -v '^$' | grep -v ':$' |
	grep -Ev '^(memcpy|memset|memmove|malloc|calloc|free|realloc|abort|exit)$' |
	grep -Ev '^(printf|fprintf|sprintf|snprintf|puts|fputs|fwrite|stderr)$' |
	grep -Ev '^(sqrt|exp|log|pow|floor|ceil|fabs|cos|sin|atan|atan2)f?$' |
	grep -Ev '^(celt_|silk_|opus_|_?_?ec_|clt_|comb_|deemphasis|resampler)' |
	sed 's/^/   /'

# Y que el punto fijo esta de verdad puesto. Si FIXED_POINT no hubiera
# llegado, opus compilaria igual en flotante y nadie se enteraria hasta
# medir el consumo en la consola.
echo
if $NM --defined-only "$OBRAS/lib/libopus.a" 2>/dev/null |
   grep -q "silk_.*_FIX"; then
	echo " punto fijo ... SI (hay simbolos silk_*_FIX)"
else
	echo " punto fijo ... NO  <<<<"
	echo "   FIXED_POINT no ha llegado al compilador. Compila igual, en"
	echo "   coma flotante, y consume mas. Mira el -I del config.h."
fi

# --- instalar ---------------------------------------------------------

rm -rf "$OUT"
mv "$OBRAS" "$OUT"
rm -f "$OUT/err.txt"
rm -rf "$OUT/obj"

echo
echo "=============================================================="
echo " listo:  $OUT"
echo "   $OUT/lib/libopus.a       $(du -h "$OUT/lib/libopus.a" | cut -f1)"
echo "   $OUT/include/opus/"
echo
echo " El Makefile ya tiene que estar apuntando ahi. Si no:"
echo "   LIBDIRS += -L$OUT/lib"
echo "   INCLUDE += -I$OUT/include"
echo "   LIBS    += -lopus"
echo "=============================================================="
