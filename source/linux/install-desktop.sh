#!/bin/sh
# Install a desktop entry for this copy of the game, for the current user only.
#
# Why a script rather than a file the build just drops in place: a .desktop entry needs
# ABSOLUTE paths for Exec and Icon, and the build output is a relocatable folder - so the
# paths are only knowable once you have put the folder where you want it. Run this from
# the game folder after moving it, and re-run it if you move it again.
#
# The FLAT and VR builds get DIFFERENT app ids, so their entries coexist instead of
# overwriting one another - they live in separate folders (build/linux/<arch> and
# <arch>vr) and each has its own copy of this script. The id is picked from the binary
# name here and must match SDL_SetAppMetadata in main.c, which picks it from AVP_PCVR.
#
# Installs per-user, so no root is needed and nothing outside $HOME is touched:
#   ~/.local/share/applications/<appid>.desktop
#   ~/.local/share/icons/hicolor/256x256/apps/<appid>.png
#
# This is also the only way the icon can appear on WAYLAND, which has no window-icon
# protocol. On X11 the in-window icon already works without this; the entry adds a menu
# item.
set -e

GAMEDIR=$(cd "$(dirname "$0")" && pwd)

# Find the game binary. avpvr_* is the VR build, avp_* the flat one.
EXEC=""
for f in "$GAMEDIR"/avpvr_* "$GAMEDIR"/avp_*; do
    case "$f" in *.bmp|*.png|*.so|*.so.*|*.txt|*.cfg|*.in|*.sh) continue ;; esac
    [ -f "$f" ] && [ -x "$f" ] && { EXEC="$f"; break; }
done
if [ -z "$EXEC" ]; then
    echo "install-desktop.sh: no game binary found in $GAMEDIR" >&2
    exit 1
fi

case "$(basename "$EXEC")" in
    avpvr_*) APPID=com.bassquake.avpvr; NAME="Aliens Versus Predator: VR" ;;
    *)       APPID=com.bassquake.avp;   NAME="Aliens Versus Predator"     ;;
esac

ICON_SRC="$GAMEDIR/avp_icon.png"
if [ ! -f "$ICON_SRC" ]; then
    echo "install-desktop.sh: $ICON_SRC missing" >&2
    exit 1
fi

APPDIR="$HOME/.local/share/applications"
ICONDIR="$HOME/.local/share/icons/hicolor/256x256/apps"
mkdir -p "$APPDIR" "$ICONDIR"

cp -f "$ICON_SRC" "$ICONDIR/$APPID.png"

sed -e "s|@EXEC@|$EXEC|g" \
    -e "s|@ICON@|$APPID|g" \
    -e "s|@GAMEDIR@|$GAMEDIR|g" \
    -e "s|@APPID@|$APPID|g" \
    -e "s|@NAME@|$NAME|g" \
    "$GAMEDIR/avp.desktop.in" > "$APPDIR/$APPID.desktop"
chmod 644 "$APPDIR/$APPID.desktop"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" || true
command -v gtk-update-icon-cache  >/dev/null 2>&1 && \
    gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1 || true

echo "Installed $APPDIR/$APPID.desktop"
echo "  Name = $NAME"
echo "  Exec = $EXEC"
