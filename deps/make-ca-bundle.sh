#!/bin/sh
#
# GR33N - generates include/ca_bundle.h from your machine's certificate
# store.
#
#   sh make-ca-bundle.sh
#
# WHY THIS ISN'T HAND-WRITTEN INTO THE REPOSITORY:
#
# A root certificate is the list of "who this console trusts". If someone
# puts the wrong certificate in there, the client accepts an impostor as
# legitimate, and nothing looks wrong: everything appears to work. It's the
# one file in the project where a mistake doesn't give you a failure, it
# gives you a hole.
#
# So the bytes are NOT copied from somewhere dubious, and not written from
# memory: they come out of your system store, the one Mozilla and your
# distribution already maintain, and this script only repackages them. If
# they ever need reviewing, run it again and compare.
#
# FILTER: by default it keeps only the roots needed to talk to Microsoft.
# Shipping all 140 from the system would work just as well, but every extra
# root is one more party the console trusts for no reason.
# To take them all: sh make-ca-bundle.sh --todas

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/../include/ca_bundle.h"

# File names (not subject names), as ca-certificates leaves them. These are
# the chains Microsoft's services go through today.
FILTRO='DigiCert_Global_Root|Baltimore_CyberTrust|Microsoft_.*Root|ISRG_Root'

SYSBUNDLE=""
for c in /etc/ssl/certs/ca-certificates.crt \
         /etc/pki/tls/certs/ca-bundle.crt \
         /etc/ssl/cert.pem; do
	[ -f "$c" ] && { SYSBUNDLE="$c"; break; }
done

if [ ! -d /etc/ssl/certs ] && [ -z "$SYSBUNDLE" ]; then
	echo "can't find the system certificate store."
	echo "on Debian/Ubuntu:  sudo apt install ca-certificates"
	exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ "$1" = "--todas" ]; then
	echo ">> every root in the system store"
	cat "$SYSBUNDLE" > "$TMP/sel.pem"
else
	echo ">> filtering by: $FILTRO"
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
		echo "   nothing matched. Check the filter, or use --todas."
		exit 1
	fi
	echo ">> $found certificates"
fi

# --- into a C header ---------------------------------------------------
#
# It's emitted as a string, not as a byte array, because mbedTLS parses PEM
# directly and this way the file can be read and eyeballed.
# The null terminator counts towards the length: mbedtls_x509_crt_parse
# requires it for PEM, and forgetting it gives you an error that says
# nothing.

{
	echo "/* GR33N - root certificates. GENERATED, do not edit by hand."
	echo " *"
	echo " * Produced by deps/make-ca-bundle.sh from the system store."
	echo " * To regenerate or review it, run that again."
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

echo ">> wrote $OUT ($(wc -c < "$OUT") bytes)"
echo
echo ">> subjects included:"
grep -c "BEGIN CERTIFICATE" "$TMP/sel.pem" | sed 's/^/   certificates: /'
awk '/BEGIN CERT/{c++} END{}' "$TMP/sel.pem" >/dev/null 2>&1 || true
openssl storeutl -noout -text "$TMP/sel.pem" 2>/dev/null \
	| grep -E "^\s*Subject:" | sed 's/^/   /' || \
	echo "   (install openssl to see the list of subjects)"
