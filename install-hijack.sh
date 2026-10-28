#!/usr/bin/env bash
# 把 portal 库直接挂到 Snipaste 的原始 ELF 上。
#
# Snipaste 会把实际运行 ELF 的路径写回桌面文件和自启动文件，所以直接修改
# /opt/snipaste/bin/Snipaste：增加 DT_NEEDED 依赖，不保留任何 ELF 副本。
set -euo pipefail

prefix="${SNIPASTE_PREFIX:-/opt/snipaste}"
bin="$prefix/bin/Snipaste"
lib="$prefix/lib/libsnipaste-portal.so"
legacy_real="$prefix/bin/Snipaste.real"
legacy_payload="$prefix/bin/Snipaste.bin"
legacy_runtime="$prefix/lib/libsnipaste-portal-required.so"

src_dir="$(dirname -- "$(readlink -f -- "$0")")"
src_lib="$src_dir/build/libsnipaste-portal.so"
pristine_bin="$src_dir/squashfs-root/usr/bin/Snipaste"

if [[ $EUID -ne 0 ]]; then
    printf '%s\n' '需要 root 权限，请用 sudo 运行。' >&2
    exit 1
fi

if [[ ! -f "$src_lib" ]]; then
    printf '%s\n' "找不到 $src_lib，请先运行 ./build.sh。" >&2
    exit 1
fi

install -m 0755 -- "$src_lib" "$lib"
printf '已安装 %s\n' "$lib"

# 从旧安装状态恢复原始 ELF，然后直接修补规范入口。
patched="$(mktemp "$prefix/bin/.Snipaste.XXXXXX")"
trap 'rm -f -- "$patched"' EXIT
if [[ "$(head -c 4 -- "$bin")" == $'\x7fELF' ]]; then
    cp --preserve=mode,timestamps -- "$bin" "$patched"
elif [[ "$(head -c 4 -- "$pristine_bin")" == $'\x7fELF' ]]; then
    cp --preserve=mode,timestamps -- "$pristine_bin" "$patched"
else
    printf '%s\n' "找不到 Snipaste 原始 ELF $bin" >&2
    exit 1
fi

patchelf --remove-needed "$lib" "$patched" 2>/dev/null || true
patchelf --remove-needed "$legacy_runtime" "$patched" 2>/dev/null || true
patchelf --add-needed "$lib" "$patched"
mv -f -- "$patched" "$bin"
trap - EXIT
printf '已为 %s 添加 portal 依赖\n' "$bin"

rm -f -- "$legacy_real" "$legacy_payload" "$legacy_runtime"

printf '%s\n' '完成。请完全退出 Snipaste 后重新启动。'
