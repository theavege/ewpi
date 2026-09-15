#!/usr/bin/env bash

function f_setup
{
    if [[ -f '/etc/os-release' ]]; then
        source '/etc/os-release'
        if ! command -v clang-tidy >/dev/null; then
            case ${ID:?} in
                msys2) return 0 ;;
                debian | ubuntu) sudo bash -c '
                    apt-get update
                    apt-get install -y clang-{tidy,format} shfmt {cpp,shell}check meson auto{conf,make} libtool gettext cmake {y,n}asm gperf python3 perl ninja-build pkgconf g++-mingw-w64-x86-64 libgomp1 gcc nsis bison flex make
                ' ;;
                fedora | alma) sudo dnf install -y shfmt {cpp,shell}check meson auto{conf,make} libtool gettext cmake {y,n}asm gperf python{,3-pip} perl ninja-build pkgconf mingw64-{libgomp,gcc,gcc-c++,nsisb} bison flex make gcc-c++ ;;
            esac 1>/dev/null
        fi
        shellcheck --external-sources "${0}"
        shfmt -ci -fn -i 4 -d "${0}"
    fi
}

set -xeuo pipefail

f_setup
declare -ar VAR=(
    --verbose
    --fatal-warnings
    --Xcc=-O3
    --cc="${CC:-clang}"
    --enable-{checking,mem-profiler,gobject-tracing}
    --pkg={gio-2.0,xlsxwriter,lib{soup-3.0,archive,git2-glib-1.0}}
)
vala  "${VAR[@]}" "${0%/*}/main.vala" --run-args 'ucrt64 mingw-w64-ucrt-x86_64-efl ../efl-staging'
