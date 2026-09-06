#!/bin/sh
#
# GR33N - prepares the folder that the XMB can launch.
#
#   sh deps/install-xmb.sh            (from the project root)
#
# Leaves in xmb/ the tree ready to copy to the console:
#
#   xmb/GR33N0PS3/PARAM.SFO
#   xmb/GR33N0PS3/ICON0.PNG
#   xmb/GR33N0PS3/USRDIR/EBOOT.BIN
#
# Copy that whole GR33N0PS3 folder to /dev_hdd0/game/ and GR33N shows up
# in the XMB as just another game. From then on, updating means swapping
# out a single file: USRDIR/EBOOT.BIN.
#
# THE NAME OF THE EXECUTABLE MATTERS. The XMB looks for USRDIR/EBOOT.BIN
# and only that. A perfectly valid gr33n.self sitting right next to it
# goes unnoticed: the entry shows up in the menu and never launches.
#
# And the TITLE_ID in PARAM.SFO has to match the folder name. If it
# doesn't, the XMB gets confused with the saved games and sometimes
# doesn't even show the entry.

set -e

: "${PS3DEV:=/usr/local/ps3dev}"

HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/xmb"

APPID=$(grep -E "^APPID" "$HERE/Makefile" | head -1 | sed 's/.*[[:space:]]//')
TITLE=$(grep -E "^TITLE" "$HERE/Makefile" | head -1 | sed 's/.*[[:space:]]//')
VER=$(grep GR33N_VERSION "$HERE/include/gr33n.h" | sed 's/.*"\(.*\)".*/\1/')

[ -n "$APPID" ] || { echo "can't find APPID in the Makefile"; exit 1; }

SELF="$HERE/gr33n.self"
[ -f "$SELF" ] || { echo "no gr33n.self. Run make first."; exit 1; }

echo ">> $TITLE $VER  ($APPID)"

rm -rf "$OUT/$APPID"
mkdir -p "$OUT/$APPID/USRDIR"

# --- PARAM.SFO -------------------------------------------------------
#
# The template is sfo.xml at the ROOT, the SAME one 'make pkg' uses via
# SFOXML. Keeping two copies of this is how XMB entries end up showing
# up in a different column depending on how you installed them.
#
# It used to be in pkgfiles/ and got moved up top, because the PSL1GHT
# rule copies the whole of pkgfiles/ INTO the package and sfo.xml ended
# up installed on the console too. This script was left pointing at the
# old spot and so broken: nobody noticed because only 'make pkg' has
# been used since.

XML="$HERE/sfo.xml"
[ -f "$XML" ] || { echo "missing sfo.xml at the project root"; exit 1; }

if [ -x "$PS3DEV/bin/sfo" ]; then
	"$PS3DEV/bin/sfo" --fromxml "$XML" "$OUT/$APPID/PARAM.SFO" \
		--title="$TITLE" --appid="$APPID" || true
fi

if [ ! -f "$OUT/$APPID/PARAM.SFO" ]; then
	echo
	echo "!! could not generate PARAM.SFO from $XML"
	echo "   To see the format this version of sfo expects:"
	echo "     $PS3DEV/bin/sfo --toxml /path/to/PARAM.SFO /tmp/example.xml"
	exit 1
fi

echo ">> PARAM.SFO from pkgfiles/sfo.xml"

# --- XMB assets ------------------------------------------------------
#
# Whatever is in pkgfiles/ under the right name gets copied. Whatever
# isn't, isn't used, and that's fine. Sizes and details are in
# pkgfiles/README.txt.
#
# And every PNG gets CHECKED before it's copied, because the XMB
# rejects them silently for two reasons you can't see just by looking
# at the image:
#
#   1. Wrong size. It doesn't scale: either it's exactly the size it
#      has to be, or it doesn't get drawn.
#   2. INTERLACED PNG (Adam7). Plenty of editors turn this on by
#      default, or leave it on when you "save for web", and the XMB's
#      decoder doesn't support it. The image looks perfect on the PC
#      and doesn't show up on the console.
#
# Both figures live in the IHDR header, at fixed positions: width at
# byte 16, height at byte 20, and the interlace mode at byte 28.

png_info() {
	# $1 file -> "width height interlace"
	#
	# The 13 bytes of the IHDR are read and assembled by hand. PNG stores
	# its integers big-endian, and od would interpret them according to
	# the machine it runs on, so doing it byte by byte is the only thing
	# that gives the same result here as anywhere else.
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
	# $1 file  $2 expected width  $3 expected height
	set -- "$1" "$2" "$3" $(png_info "$1")
	w=$4; h=$5; il=$6

	[ -n "$w" ] || { echo "   (doesn't look like a PNG)"; return 1; }

	ok=0
	if [ "$w" != "$2" ] || [ "$h" != "$3" ]; then
		echo "   !! it's ${w}x${h}, it has to be ${2}x${3}"
		ok=1
	fi
	if [ "$il" != "0" ]; then
		echo "   !! it's INTERLACED. The XMB won't draw it."
		echo "      Fix:  magick \"$1\" -interlace none \"$1\""
		ok=1
	fi

	[ "$ok" -eq 0 ] && echo "   ${w}x${h}, not interlaced"
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

# With no icon of its own, the PSL1GHT generic one. Ugly, but without
# ICON0.PNG some firmwares just won't draw the entry at all.
if [ ! -f "$OUT/$APPID/ICON0.PNG" ]; then
	for g in "$PS3DEV/bin/ICON0.PNG" \
	         "$PS3DEV/ppu/ICON0.PNG" \
	         "$PS3DEV/psl1ght/ICON0.PNG"; do
		if [ -f "$g" ]; then
			cp "$g" "$OUT/$APPID/ICON0.PNG"
			echo ">> generic ICON0.PNG ($g)"
			break
		fi
	done
fi

if [ ! -f "$OUT/$APPID/ICON0.PNG" ]; then
	echo ">> NO ICON. Drop a 320x176 one in pkgfiles/ICON0.PNG"
fi

# --- the executable, with the NAME the XMB looks for -----------------

cp "$SELF" "$OUT/$APPID/USRDIR/EBOOT.BIN"

echo
echo ">> ready in $OUT/$APPID"
find "$OUT/$APPID" -type f | sed "s|$OUT/|   |"
echo
echo "   Copy the $APPID folder to /dev_hdd0/game/ on the console."
echo "   To update afterwards: just USRDIR/EBOOT.BIN."
echo
echo "   PIC1.PNG and PIC0.PNG live at the ROOT of the folder, next to"
echo "   PARAM.SFO - not inside USRDIR. And the XMB caches: if you change"
echo "   an image and keep seeing the old one, restart the console."
