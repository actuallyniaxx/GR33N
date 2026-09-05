#!/bin/sh
#
# GR33N - prepara la carpeta que el XMB puede lanzar.
#
#   sh deps/install-xmb.sh            (desde la raiz del proyecto)
#
# Deja en xmb/ el arbol listo para copiar a la consola:
#
#   xmb/GR33N0PS3/PARAM.SFO
#   xmb/GR33N0PS3/ICON0.PNG
#   xmb/GR33N0PS3/USRDIR/EBOOT.BIN
#
# Copia esa carpeta GR33N0PS3 entera a /dev_hdd0/game/ y GR33N sale en el
# XMB como un juego mas. A partir de ahi, actualizar es sustituir un solo
# fichero: USRDIR/EBOOT.BIN.
#
# EL NOMBRE DEL EJECUTABLE IMPORTA. El XMB busca USRDIR/EBOOT.BIN y solo
# eso. Un gr33n.self perfectamente valido al lado no lo mira nadie: la
# entrada aparece en el menu y no arranca.
#
# Y el TITLE_ID del PARAM.SFO tiene que coincidir con el nombre de la
# carpeta. Si no, el XMB se lia con las partidas guardadas y a veces ni
# muestra la entrada.

set -e

: "${PS3DEV:=/usr/local/ps3dev}"

HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/xmb"

APPID=$(grep -E "^APPID" "$HERE/Makefile" | head -1 | sed 's/.*[[:space:]]//')
TITLE=$(grep -E "^TITLE" "$HERE/Makefile" | head -1 | sed 's/.*[[:space:]]//')
VER=$(grep GR33N_VERSION "$HERE/include/gr33n.h" | sed 's/.*"\(.*\)".*/\1/')

[ -n "$APPID" ] || { echo "no encuentro APPID en el Makefile"; exit 1; }

SELF="$HERE/gr33n.self"
[ -f "$SELF" ] || { echo "no hay gr33n.self. Ejecuta make primero."; exit 1; }

echo ">> $TITLE $VER  ($APPID)"

rm -rf "$OUT/$APPID"
mkdir -p "$OUT/$APPID/USRDIR"

# --- PARAM.SFO -------------------------------------------------------
#
# La plantilla es sfo.xml de la RAIZ, LA MISMA que usa 'make pkg' via
# SFOXML. Tener dos copias de esto es como acaban las entradas del XMB
# apareciendo en una columna distinta segun como las hayas instalado.
#
# Estuvo en pkgfiles/ y se movio arriba, porque la regla de PSL1GHT copia
# pkgfiles/ entero DENTRO del paquete y el sfo.xml acababa instalado en la
# consola. Este script se quedo apuntando al sitio viejo y por tanto
# roto: no se noto porque desde entonces solo se ha usado 'make pkg'.

XML="$HERE/sfo.xml"
[ -f "$XML" ] || { echo "falta sfo.xml en la raiz del proyecto"; exit 1; }

if [ -x "$PS3DEV/bin/sfo" ]; then
	"$PS3DEV/bin/sfo" --fromxml "$XML" "$OUT/$APPID/PARAM.SFO" \
		--title="$TITLE" --appid="$APPID" || true
fi

if [ ! -f "$OUT/$APPID/PARAM.SFO" ]; then
	echo
	echo "!! no se pudo generar PARAM.SFO desde $XML"
	echo "   Para ver el formato que espera esta version de sfo:"
	echo "     $PS3DEV/bin/sfo --toxml /ruta/a/PARAM.SFO /tmp/ejemplo.xml"
	exit 1
fi

echo ">> PARAM.SFO desde pkgfiles/sfo.xml"

# --- recursos del XMB ------------------------------------------------
#
# Lo que este en pkgfiles/ con el nombre correcto se copia. Lo que no
# este, no se usa y no pasa nada. Tamanos y detalles en
# pkgfiles/README.txt.
#
# Y se COMPRUEBA cada PNG antes de copiarlo, porque el XMB los rechaza en
# silencio por dos motivos que no se ven mirando la imagen:
#
#   1. Tamano equivocado. No escala: o mide lo que tiene que medir o no
#      se dibuja.
#   2. PNG ENTRELAZADO (Adam7). Muchos editores lo activan por defecto o
#      lo dejan puesto al "guardar para web", y el decodificador del XMB
#      no lo admite. La imagen se ve perfecta en el PC y no aparece en la
#      consola.
#
# Los dos datos estan en la cabecera IHDR, en posiciones fijas: ancho en
# el byte 16, alto en el 20, y el modo de entrelazado en el 28.

png_info() {
	# $1 fichero -> "ancho alto entrelazado"
	#
	# Se leen los 13 bytes del IHDR y se componen a mano. El PNG guarda
	# los enteros en big-endian y od los interpretaria segun la maquina,
	# asi que byte a byte es lo unico que da el mismo resultado aqui y en
	# cualquier sitio.
	od -An -tu1 -j16 -N13 "$1" 2>/dev/null | tr -s ' ' '\n' | grep -v '^$' | awk '
		{ v[NR] = $1 }
		END {
			if (NR < 13) exit 1
			w = v[1]*16777216 + v[2]*65536 + v[3]*256 + v[4]
			h = v[5]*16777216 + v[6]*65536 + v[7]*256 + v[8]
			printf "%d %d %d\n", w, h, v[13]
		}'
}

check_png() {
	# $1 fichero  $2 ancho esperado  $3 alto esperado
	set -- "$1" "$2" "$3" $(png_info "$1")
	w=$4; h=$5; il=$6

	[ -n "$w" ] || { echo "   (no parece un PNG)"; return 1; }

	ok=0
	if [ "$w" != "$2" ] || [ "$h" != "$3" ]; then
		echo "   !! mide ${w}x${h}, tiene que medir ${2}x${3}"
		ok=1
	fi
	if [ "$il" != "0" ]; then
		echo "   !! esta ENTRELAZADO. El XMB no lo va a dibujar."
		echo "      Arreglo:  magick \"$1\" -interlace none \"$1\""
		ok=1
	fi

	[ "$ok" -eq 0 ] && echo "   ${w}x${h}, sin entrelazar"
	return $ok
}

for f in ICON0.PNG PIC0.PNG PIC1.PNG SND0.AT3 ICON1.PAM; do
	src="$HERE/pkgfiles/$f"
	[ -f "$src" ] || continue

	echo ">> $f"

	case $f in
		ICON0.PNG) check_png "$src" 320 176  || true ;;
		PIC0.PNG)  check_png "$src" 1000 560 || true ;;
		PIC1.PNG)  check_png "$src" 1920 1080 || true ;;
	esac

	cp "$src" "$OUT/$APPID/$f"
done

# Sin icono propio, el generico de PSL1GHT. Feo, pero sin ICON0.PNG hay
# firmwares que directamente no dibujan la entrada.
if [ ! -f "$OUT/$APPID/ICON0.PNG" ]; then
	for g in "$PS3DEV/bin/ICON0.PNG" \
	         "$PS3DEV/ppu/ICON0.PNG" \
	         "$PS3DEV/psl1ght/ICON0.PNG"; do
		if [ -f "$g" ]; then
			cp "$g" "$OUT/$APPID/ICON0.PNG"
			echo ">> ICON0.PNG generico ($g)"
			break
		fi
	done
fi

if [ ! -f "$OUT/$APPID/ICON0.PNG" ]; then
	echo ">> SIN ICONO. Deja uno de 320x176 en pkgfiles/ICON0.PNG"
fi

# --- el ejecutable, con el NOMBRE que busca el XMB -------------------

cp "$SELF" "$OUT/$APPID/USRDIR/EBOOT.BIN"

echo
echo ">> listo en $OUT/$APPID"
find "$OUT/$APPID" -type f | sed "s|$OUT/|   |"
echo
echo "   Copia la carpeta $APPID a /dev_hdd0/game/ de la consola."
echo "   Para actualizar despues: solo USRDIR/EBOOT.BIN."
echo
echo "   PIC1.PNG y PIC0.PNG van en la RAIZ de la carpeta, junto al"
echo "   PARAM.SFO - no dentro de USRDIR. Y el XMB cachea: si cambias"
echo "   una imagen y sigues viendo la vieja, reinicia la consola."
