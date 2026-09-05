#!/bin/sh
#
# GR33N - genera include/ca_bundle.h a partir del almacen de certificados
# de tu maquina.
#
#   sh make-ca-bundle.sh
#
# POR QUE ESTO NO VIENE ESCRITO A MANO EN EL REPOSITORIO:
#
# Un certificado raiz es la lista de "en quien confia esta consola". Si
# alguien mete ahi un certificado equivocado, el cliente acepta como
# legitimo a quien no lo es, y no se nota: todo parece funcionar. Es el
# unico fichero del proyecto donde un error no da un fallo, da un agujero.
#
# Asi que los bytes NO se copian de ningun sitio dudoso ni se escriben de
# memoria: salen del almacen de tu sistema, que es el que ya mantienen
# Mozilla y tu distribucion, y este script solo los reempaqueta. Si alguna
# vez hay que revisarlos, se vuelve a ejecutar y se compara.
#
# FILTRO: por defecto solo se quedan las raices que hacen falta para
# hablar con Microsoft. Meter las 140 del sistema funcionaria igual, pero
# cada raiz de mas es alguien mas en quien la consola confia sin motivo.
# Para llevarlas todas: sh make-ca-bundle.sh --todas

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/../include/ca_bundle.h"

# Nombres de fichero (no de sujeto) tal y como los deja ca-certificates.
# Son las cadenas por las que pasan hoy los servicios de Microsoft.
FILTRO='DigiCert_Global_Root|Baltimore_CyberTrust|Microsoft_.*Root|ISRG_Root'

SYSBUNDLE=""
for c in /etc/ssl/certs/ca-certificates.crt \
         /etc/pki/tls/certs/ca-bundle.crt \
         /etc/ssl/cert.pem; do
	[ -f "$c" ] && { SYSBUNDLE="$c"; break; }
done

if [ ! -d /etc/ssl/certs ] && [ -z "$SYSBUNDLE" ]; then
	echo "no encuentro el almacen de certificados del sistema."
	echo "en Debian/Ubuntu:  sudo apt install ca-certificates"
	exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ "$1" = "--todas" ]; then
	echo ">> todas las raices del sistema"
	cat "$SYSBUNDLE" > "$TMP/sel.pem"
else
	echo ">> filtrando por: $FILTRO"
	found=0
	for f in /etc/ssl/certs/*.pem; do
		[ -f "$f" ] || continue
		case $(basename "$f") in
			*) ;;
		esac
		if basename "$f" | grep -qE "$FILTRO"; then
			echo "   + $(basename "$f")"
			cat "$f" >> "$TMP/sel.pem"
			found=$((found + 1))
		fi
	done
	if [ "$found" -eq 0 ]; then
		echo "   ninguna coincide. Repasa el filtro o usa --todas."
		exit 1
	fi
	echo ">> $found certificados"
fi

# --- a cabecera C ------------------------------------------------------
#
# Se emite como cadena, no como array de bytes, porque mbedTLS parsea PEM
# directamente y asi el fichero se puede leer y comprobar a ojo.
# El terminador nulo cuenta en la longitud: mbedtls_x509_crt_parse lo
# exige para PEM, y olvidarlo da un error que no dice nada.

{
	echo "/* GR33N - certificados raiz. GENERADO, no editar a mano."
	echo " *"
	echo " * Lo produce deps/make-ca-bundle.sh desde el almacen del sistema."
	echo " * Para regenerarlo o revisarlo, vuelve a ejecutarlo."
	echo " */"
	echo
	echo "#ifndef GR33N_CA_BUNDLE_H"
	echo "#define GR33N_CA_BUNDLE_H"
	echo
	echo "static const char ca_bundle_pem[] ="
	sed 's/\\/\\\\/g; s/"/\\"/g; s/^/\t"/; s/$/\\n"/' "$TMP/sel.pem"
	echo "\t;"
	echo
	echo "#endif /* GR33N_CA_BUNDLE_H */"
} > "$OUT"

echo ">> escrito $OUT ($(wc -c < "$OUT") bytes)"
echo
echo ">> sujetos incluidos:"
grep -c "BEGIN CERTIFICATE" "$TMP/sel.pem" | sed 's/^/   certificados: /'
awk '/BEGIN CERT/{c++} END{}' "$TMP/sel.pem" >/dev/null 2>&1 || true
openssl storeutl -noout -text "$TMP/sel.pem" 2>/dev/null \
	| grep -E "^\s*Subject:" | sed 's/^/   /' || \
	echo "   (instala openssl para ver la lista de sujetos)"
