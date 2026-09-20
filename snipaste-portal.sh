#!/usr/bin/env bash
set -euo pipefail
base_dir="$(dirname -- "$(readlink -f -- "$0")")"
if [[ ! -f "$base_dir/build/libsnipaste-portal.so" ]]; then
    printf '%s\n' 'Run ./build.sh first.' >&2
    exit 1
fi
export APPDIR="$base_dir/squashfs-root"
export APPIMAGE="$base_dir/snipaste-2.11.3.AppImage"
export OWD="$PWD"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_PRELOAD="$base_dir/build/libsnipaste-portal.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec "$APPDIR/AppRun" "$@"
