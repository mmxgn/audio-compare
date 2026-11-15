#!/bin/sh
# Bundle launcher: point GLib, GTK, GStreamer and fontconfig at the files
# inside the .app, then exec the real binary (same process, so the Dock icon
# and Finder "Open With" events belong to it).
MACOS="$(cd "$(dirname "$0")" && pwd)"
CONTENTS="$(dirname "$MACOS")"
RES="$CONTENTS/Resources"
CACHE="$HOME/Library/Caches/org.mmxgn.audiocompare"
mkdir -p "$CACHE"

export XDG_DATA_DIRS="$RES/share"
export GSETTINGS_SCHEMA_DIR="$RES/share/glib-2.0/schemas"
export GIO_MODULE_DIR="$RES/gio-modules"
export FONTCONFIG_FILE="$RES/etc/fonts/fonts.conf"

# Only the bundled plugins, with a registry of our own so a Homebrew
# GStreamer on the same machine never mixes in.
unset GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0
export GST_PLUGIN_SYSTEM_PATH_1_0="$RES/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER_1_0="$MACOS/gst-plugin-scanner"
export GST_REGISTRY_1_0="$CACHE/gst-registry-$(uname -m).bin"

# loaders.cache needs absolute paths, so fill in where the app lives now.
sed "s|@LIB@|$RES/lib|g" "$RES/gdk-pixbuf-loaders.cache.in" > "$CACHE/loaders.cache"
export GDK_PIXBUF_MODULE_FILE="$CACHE/loaders.cache"

exec "$MACOS/audio-compare-bin" "$@"
