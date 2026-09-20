#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$(readlink -f -- "$0")")"
mkdir -p build .cache/tmp
export TMPDIR="$PWD/.cache/tmp"
qt_dir="${SNIPASTE_QT_DIR:-$PWD/qt/6.2.4/gcc_64}"
app_dir="$PWD/squashfs-root"
if [[ ! -f "$app_dir/usr/lib/libQt6Core.so.6" ]]; then
    ./snipaste-2.11.3.AppImage --appimage-extract
fi
if [[ ! -x "$qt_dir/libexec/moc" ]]; then
    printf '%s\n' 'Missing Qt 6.2.4. Install with:' \
        'TMPDIR="$PWD/.cache/tmp" UV_TOOL_DIR="$PWD/.cache/uv-tools" uvx --cache-dir .cache/uv --from aqtinstall aqt install-qt linux desktop 6.2.4 gcc_64 -O qt --archives qtbase' >&2
    exit 1
fi
export LD_LIBRARY_PATH="$app_dir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
includes=(-I"$qt_dir/include" -I"$qt_dir/include/QtCore" -I"$qt_dir/include/QtGui"
          -I"$qt_dir/include/QtWidgets" -I"$qt_dir/include/QtDBus")
"$qt_dir/libexec/moc" "${includes[@]}" portal-dialog.cpp -o build/portal-dialog.moc
"${CXX:-g++}" -std=c++17 -O2 -fPIC -shared -Wall -Wextra \
    "${includes[@]}" -Ibuild portal-dialog.cpp -o build/libsnipaste-portal.so.new \
    -L"$app_dir/usr/lib" -Wl,-z,defs -Wl,--version-script=exports.map \
    -Wl,-rpath,'$ORIGIN/../squashfs-root/usr/lib' \
    -l:libQt6Widgets.so.6 -l:libQt6Gui.so.6 -l:libQt6DBus.so.6 -l:libQt6Core.so.6 -ldl
mv -f build/libsnipaste-portal.so.new build/libsnipaste-portal.so
printf '%s\n' 'Built build/libsnipaste-portal.so' 'Launch: ./snipaste-portal.sh'
