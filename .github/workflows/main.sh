#!/usr/bin/env bash

function f_setup
{
    if [[ -f '/etc/os-release' ]]; then
        source '/etc/os-release'
        if ! command -v valac >/dev/null; then
            case ${ID:?} in
                msys2) return 0 ;;
                debian | ubuntu) sudo bash -c '
                    apt-get update
                    apt-get install -y ca-certificates glib-networking valac clang-{tidy,format} shfmt {cpp,shell}check meson auto{conf,make} {lib,its}tool gettext cmake {y,n}asm gperf python3 perl ninja-build pkgconf g++-mingw-w64-x86-64 libgomp1 gcc nsis bison flex make lib{archive,soup-3.0,gee-0.8,git2-glib-1.0}-dev
                    update-ca-certificates --fresh --verbose
                ' ;;
                fedora | alma) sudo dnf install -y shfmt {cpp,shell}check meson auto{conf,make} {lib,its}tool gettext cmake {y,n}asm gperf python{,3-pip} perl ninja-build pkgconf mingw32-{libgomp,gcc,gcc-c++,nsisb} bison flex make gcc-c++ lib{archive,git2-glib}-devel;;
            esac 1>/dev/null
        fi
        #~ shellcheck --external-sources "${0}" packages/*/*.sh
        #~ shfmt -ci -fn -i 4 -d "${0}" packages/*/*.sh
        #~ clang-tidy ewpi*.{c,h}
        #~ clang-format --dry-run --Werror -style=Mozilla ewpi*.{c,h}
    fi
}

set -xeuo pipefail

f_setup
#~ command -v meson make{,nsis} python perl ninja {y,n}asm gperf wget bison flex itstool
#~ gcc -W{error,all,extra,pedantic,shadow,conversion} -std=c99 -O2 -o ewpi{,*.c}

declare -ar VAR=(
    --verbose
    --fatal-warnings
    --Xcc=-O3
    --cc=clang
    --enable-{checking,mem-profiler,gobject-tracing}
    --pkg={gio-2.0,lib{soup-3.0,archive,git2-glib-1.0}}
)

vala  "${VAR[@]}" src/main.vala - --efl --insecure --verbose
