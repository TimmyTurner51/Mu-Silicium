#!/bin/bash
# Brand the Mu-Silicium boot graphics with Kettle Linux art.
#
# The art comes from kettlelinux packages/kettle-branding/art (one source of truth for the
# Plymouth splash, the Plasma splash, the Steam startup movie and this): a copper kettle and
# a wordmark drawn as plain strokes, so neither build nor boot needs a font.
#
# Sizing is not free-form. MsGraphicsPkg's DisplayBootGraphic() draws the system logo at
# native size, centred, and REFUSES it outright (EFI_INVALID_PARAMETER, nothing drawn) if
# Width > 40% of the panel or Height > 40% of it. The nx729j panel is 1116x2480, so the
# ceiling is 446x992. 400x540 sits safely under that and also survives a 1080x2400 panel
# (40% = 432x960).
#
# The screen background is filled with GetBackgroundColor(), which this platform returns as
# 0 (black), so the bitmaps are composed on pure black and blend invisibly into it.
set -euo pipefail

ART="${1:?kettle-branding art dir}"
RES="${2:?Mu resources dir}"
WORK="${3:-$(mktemp -d)}"
mkdir -p "$WORK"

echo "art: $ART"
echo "res: $RES"
for f in kettle.svg wordmark.svg; do
    [ -f "$ART/$f" ] || { echo "missing $ART/$f" >&2; exit 1; }
done

# ---- the mark, at 320 px (the size the Plymouth splash uses) -------------------
# Trimmed: the SVG's viewBox has empty space above the handle and right of the spout,
# and leaving it in would push the visible group off-centre. Rendered larger than the
# splash's 320 px because the panel gives the boot logo far more room than that.
KREW=${KREW:-420}   # kettle render width  -> trims to roughly 330 px wide
WMREW=${WMREW:-360} # wordmark render width
GAP=${GAP:-44}
MARGINX=${MARGINX:-34}
MARGINY=${MARGINY:-40}
rsvg-convert -w "$KREW" "$ART/kettle.svg" -o "$WORK/kettle.png"
rsvg-convert -w "$WMREW" "$ART/wordmark.svg" -o "$WORK/wordmark.png"
magick "$WORK/kettle.png"   -trim +repage "$WORK/kettle.png"
magick "$WORK/wordmark.png" -trim +repage "$WORK/wordmark.png"

KW=$(magick identify -format '%w' "$WORK/kettle.png")
KH=$(magick identify -format '%h' "$WORK/kettle.png")
WW=$(magick identify -format '%w' "$WORK/wordmark.png")
WH=$(magick identify -format '%h' "$WORK/wordmark.png")
echo "kettle ${KW}x${KH}  wordmark ${WW}x${WH}"

# kettle, GAP, wordmark, centred as one group on black. The canvas hugs the content so
# there is no dead margin to reason about, and is checked against the 40% rule below.
CANW=$(( (KW > WW ? KW : WW) + 2*MARGINX ))
GROUPH=$((KH + GAP + WH))
CANH=$(( GROUPH + 2*MARGINY ))
OY=$MARGINY
KX=$(( (CANW - KW) / 2 ))
WX=$(( (CANW - WW) / 2 ))
WY=$(( OY + KH + GAP ))
echo "canvas ${CANW}x${CANH}  group h=${GROUPH}  top=${OY}"

# The 40% ceiling DisplayBootGraphic() enforces, for this panel and the 1080x2400 fallback.
#
# BootGraphicsLib.c only refuses BG_SYSTEM_LOGO outside 40% of each axis, returning
# EFI_INVALID_PARAMETER and drawing NOTHING -- so an oversized bitmap is not cosmetic, it means
# a blank screen. Both canvases have to be tested: an earlier revision checked only the boot
# logo and shipped a 473x476 NoBootOS.bmp against a 446 px ceiling.
#
# The tighter of the two panels is used so one asset is valid on either.
MAXW=0; MAXH=0
for panel in "1116 2480" "1080 2400"; do
    set -- $panel
    maxw=$(( $1 * 40 / 100 )); maxh=$(( $2 * 40 / 100 ))
    [ "$MAXW" -eq 0 ] || [ "$maxw" -lt "$MAXW" ] && MAXW=$maxw
    [ "$MAXH" -eq 0 ] || [ "$maxh" -lt "$MAXH" ] && MAXH=$maxh
done
echo "  ceiling across panels: ${MAXW}x${MAXH}"

fits() { [ "$1" -le "$MAXW" ] && [ "$2" -le "$MAXH" ]; }

check_canvas() { # name w h
    if ! fits "$2" "$3"; then
        echo "ERROR: $1 is ${2}x${3}, over the ${MAXW}x${MAXH} ceiling;" \
             "DisplayBootGraphic would refuse to draw it" >&2
        return 1
    fi
    echo "  $1 ${2}x${3} fits"
}

check_canvas BootLogo "$CANW" "$CANH" || { echo "refusing to emit an unusable BootLogo.bmp" >&2; exit 1; }
magick -size ${CANW}x${CANH} xc:black \
    "$WORK/kettle.png" -geometry +${KX}+${OY} -composite \
    "$WORK/wordmark.png" -geometry +${WX}+${WY} -composite \
    -alpha off -type TrueColor "BMP3:$WORK/BootLogo.bmp"

# ---- no bootable OS: the same mark, with the reason under it ------------------
# The message is rasterised here, at build time, so nothing at boot needs a font.
MSGPOINTS=${MSGPOINTS:-30}
MSGAP=${MSGAP:-30}
magick -background none -fill '#F4EFE9' -font DejaVu-Sans -pointsize "$MSGPOINTS" \
    -gravity center label:'No bootable device' "$WORK/msg.png"
magick "$WORK/msg.png" -trim +repage "$WORK/msg.png"

MW=$(magick identify -format '%w' "$WORK/msg.png")
MH=$(magick identify -format '%h' "$WORK/msg.png")

# The canvas width is max(boot-logo width, message width) + margins, so a wide label is what
# pushes it past the ceiling. Shrink the label until the canvas fits instead of emitting a
# bitmap the firmware will silently discard.
# CANW already includes 2*MARGINX (it is max(KW,WW) + 2*MARGINX), so padding must not be
# added again on top of it -- doing so put a 473 px canvas against a 432 px ceiling.
CAN2W=$(( CANW > MW + 2*MARGINX ? CANW : MW + 2*MARGINX ))
GROUP2H=$((KH + GAP + WH + MSGAP + MH))
CAN2H=$(( GROUP2H + 2*MARGINY ))
while ! fits "$CAN2W" "$CAN2H" && [ "$MSGPOINTS" -gt 10 ]; do
    MSGPOINTS=$(( MSGPOINTS - 2 ))
    magick -background none -fill '#F4EFE9' -font DejaVu-Sans -pointsize "$MSGPOINTS" \
        -gravity center label:'No bootable device' "$WORK/msg.png"
    magick "$WORK/msg.png" -trim +repage "$WORK/msg.png"
    MW=$(magick identify -format '%w' "$WORK/msg.png")
    MH=$(magick identify -format '%h' "$WORK/msg.png")
    # CANW already includes 2*MARGINX (it is max(KW,WW) + 2*MARGINX), so padding must not be
# added again on top of it -- doing so put a 473 px canvas against a 432 px ceiling.
CAN2W=$(( CANW > MW + 2*MARGINX ? CANW : MW + 2*MARGINX ))
    GROUP2H=$((KH + GAP + WH + MSGAP + MH))
    CAN2H=$(( GROUP2H + 2*MARGINY ))
    echo "  no-boot too wide; message shrunk to ${MSGPOINTS} pt -> canvas ${CAN2W}x${CAN2H}"
done
check_canvas NoBootOS "$CAN2W" "$CAN2H" || { echo "refusing to emit an unusable NoBootOS.bmp" >&2; exit 1; }
# same content-sized, equally-margined layout as the boot logo
magick -size ${CAN2W}x${CAN2H} xc:black \
    "$WORK/kettle.png"   -geometry +$(( (CAN2W-KW)/2 ))+${MARGINY} -composite \
    "$WORK/wordmark.png" -geometry +$(( (CAN2W-WW)/2 ))+$(( MARGINY + KH + GAP )) -composite \
    "$WORK/msg.png"      -geometry +$(( (CAN2W-MW)/2 ))+$(( MARGINY + KH + GAP + WH + MSGAP )) -composite \
    -alpha off -type TrueColor "BMP3:$WORK/NoBootOS.bmp"

echo
echo "=== produced ==="
for f in BootLogo.bmp NoBootOS.bmp; do
    identify -format "  %f  %wx%h  depth=%z  type=%[type]\n" "$WORK/$f"
    ls -l "$WORK/$f" | awk '{print "    bytes="$5}'
done

# ---------------------------------------------------------------------------
# Install only if explicitly asked; -i means "write them into the FDF's paths".
if [ "${INSTALL:-0}" = "1" ]; then
    for f in BootLogo.bmp NoBootOS.bmp; do
        if [ -f "$RES/$f" ] && [ ! -f "$RES/$f.stock" ]; then
            cp -p "$RES/$f" "$RES/$f.stock"
            echo "  kept stock $f as $f.stock"
        fi
        cp "$WORK/$f" "$RES/$f"
        echo "  installed $RES/$f"
    done
else
    echo "(dry run: set INSTALL=1 to write into $RES)"
fi
