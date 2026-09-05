#!/bin/sh
#
# GR33N - las pruebas que se corren en el PC
#
#   sh t/correr.sh            todas
#   sh t/correr.sh audio       una sola
#
# ------------------------------------------------------------------
# QUE SE PRUEBA AQUI Y QUE NO
# ------------------------------------------------------------------
#
# Aqui NO se prueba la PS3. Se prueba la ARITMETICA: el reordenado de
# paquetes, el anillo de muestras, el mapeo del mando, la zona muerta,
# el troceado de NALs, el parseo de mensajes. Todo eso es codigo que da
# el mismo resultado en un PC que en una consola, y en un PC se puede
# correr bajo ASan y UBSan, que en la PS3 no existen.
#
# Lo que depende de la consola de verdad --el descodificador, el RSX, la
# pila de red de lv2-- no se prueba aqui y no se puede: se prueba
# encendiendo la consola. Que estas pruebas pasen NO quiere decir que
# GR33N funcione. Quiere decir que si falla, no es por esto.
#
# ------------------------------------------------------------------
# POR QUE EXISTEN
# ------------------------------------------------------------------
#
# Por el anillo de PCM de aud.c. Estaba dimensionado a 8192 pares, y con
# una trama de Opus de hasta 5760 quedaban 2431 pares utiles --unos 50 ms
# de sonido-- para un colchon de 160. El audio no habria arrancado NUNCA,
# y ninguna pieza estaba rota: cada una hacia su trabajo y el resultado
# era silencio. Eso no se ve leyendo. Lo canto esta prueba en la primera
# pasada, antes de compilar nada para la consola.
#
# Cada ronda de "mandar a la consola, copiar el PKG, arrancar, mirar"
# son diez minutos. Esto son cuatro segundos.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
RAIZ=$(cd "$HERE/.." && pwd)

CC=${CC:-gcc}
BASE="-std=c99 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -g"

# ASan y UBSan si estan; si no, se corre igual y se dice.
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
if ! echo 'int main(void){return 0;}' | $CC -xc - $SAN -o /dev/null 2>/dev/null; then
	echo "   (sin ASan/UBSan en este compilador: se corre sin ellos)"
	SAN=""
fi

# ------------------------------------------------------------------
# LAS RECETAS
# ------------------------------------------------------------------
#
# La mayoria de pruebas se bastan solas: copian la funcion bajo prueba o
# la incluyen. Las cuatro de abajo compilan codigo DE VERDAD de source/,
# y por eso llevan receta propia.
#
# Van aqui, en una sola tabla, y no repartidas en variables sueltas: dos
# sitios donde apuntar el mismo dato acaban discrepando. Es la misma
# razon por la que i18n.h no tiene dos listas.
receta()
{
	case "$1" in
	audio)      echo "../source/aud.c falso/plataforma.c falso/audio_falso.c -lpthread" ;;
	region)     echo "../source/ping.c falso/plataforma.c -lpthread" ;;
	dtls_timer) echo "../source/dtls_timer.c" ;;
	idioma)     echo "../source/i18n.c" ;;
	mando)      echo "../source/xcmsg.c" ;;
	peerlog)    echo "../source/peer_log.c" ;;
	teredo)     echo "../source/teredo.c" ;;
	vjitter)    echo "../source/vjitter.c" ;;
	xcmsg)      echo "../source/xcmsg.c" ;;
	*)          echo "" ;;
	esac
}

cd "$HERE"

if [ $# -gt 0 ]; then
	LISTA="$*"
else
	LISTA=$(ls *.c | sed 's/\.c$//' | tr '\n' ' ')
fi

echo "=============================================================="
echo " GR33N - pruebas en el PC"
echo "=============================================================="
echo

mkdir -p .bin
malas=""
n=0

for t in $LISTA; do
	[ -f "$t.c" ] || { echo "   no existe t/$t.c"; malas="$malas $t"; continue; }

	n=$((n + 1))
	extra=$(receta "$t")

	# shellcheck disable=SC2086
	if ! $CC $BASE $SAN -I. -Ifalso -Istub -I"$RAIZ/include" -o ".bin/$t" "$t.c" $extra 2>".bin/$t.cc"; then
		echo "--- $t: NO COMPILA ---"
		sed 's/^/      /' ".bin/$t.cc"
		malas="$malas $t"
		continue
	fi

	echo "--- $t ---"
	if "./.bin/$t"; then
		:
	else
		echo "   ^^ $t TERMINA EN FALLO"
		malas="$malas $t"
	fi
	echo
done

echo "=============================================================="
if [ -n "$malas" ]; then
	echo " FALLAN:$malas"
	echo " ($n pruebas intentadas)"
	exit 1
fi
echo " las $n pruebas pasan."
