#!/usr/bin/env bash
# 把系统安装的 Snipaste 换成 portal 劫持版本。
#
# Snipaste 启动时会用自带模板重写 ~/.local/share/applications/_snipaste.desktop
# 和 ~/.config/autostart/_snipaste.desktop，但 Exec 永远指向
# /opt/snipaste/bin/Snipaste。把这个二进制换成 wrapper 之后，无论桌面文件
# 被怎么改，都会命中我们的 LD_PRELOAD，不需要再 chattr +i（那会让它启动失败）。
set -euo pipefail

prefix="${SNIPASTE_PREFIX:-/opt/snipaste}"
bin="$prefix/bin/Snipaste"
real="$prefix/bin/Snipaste.real"
lib="$prefix/lib/libsnipaste-portal.so"

src_dir="$(dirname -- "$(readlink -f -- "$0")")"
src_lib="$src_dir/build/libsnipaste-portal.so"

if [[ $EUID -ne 0 ]]; then
    printf '%s\n' '需要 root 权限，请用 sudo 运行。' >&2
    exit 1
fi

if [[ ! -f "$src_lib" ]]; then
    printf '%s\n' "找不到 $src_lib，请先运行 ./build.sh。" >&2
    exit 1
fi

# 备份真实二进制，幂等：只有当前 $bin 还是 ELF 时才改名。
if [[ ! -e "$real" ]]; then
    if [[ "$(head -c 4 -- "$bin")" != $'\x7fELF' ]]; then
        printf '%s\n' "$bin 不是 ELF 且缺少 $real，安装状态异常，已中止。" >&2
        exit 1
    fi
    mv -f -- "$bin" "$real"
    printf '已备份 %s -> %s\n' "$bin" "$real"
fi

install -m 0755 -- "$src_lib" "$lib"
printf '已安装 %s\n' "$lib"

cat > "$bin" <<EOF
#!/bin/sh
# 由 install-hijack.sh 生成，请勿手动修改。
export LD_PRELOAD="$lib\${LD_PRELOAD:+:\$LD_PRELOAD}"
exec "$real" "\$@"
EOF
chmod 0755 -- "$bin"
printf '已写入 wrapper %s\n' "$bin"

# /usr/bin/Snipaste 现在只负责转发，LD_PRELOAD 由 wrapper 统一设置。
if [[ -e /usr/bin/Snipaste ]]; then
    cat > /usr/bin/Snipaste <<'EOF'
#!/bin/sh
export APPIMAGE="/usr/bin/Snipaste"
exec /opt/snipaste/bin/Snipaste "$@"
EOF
    chmod 0755 -- /usr/bin/Snipaste
    printf '%s\n' '已更新 /usr/bin/Snipaste'
fi

printf '%s\n' '完成。请完全退出 Snipaste 后重新启动。'
