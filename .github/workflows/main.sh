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
                    apt-get install -y sh{fmt,ellcheck} nsis valac lib{archive,soup-3.0,git2-glib-1.0}-dev
                ' ;;
                fedora | alma) sudo dnf install -y shfmt {cpp,shell}check meson auto{conf,make} libtool gettext cmake {y,n}asm gperf python{,3-pip} perl ninja-build pkgconf mingw64-{libgomp,gcc,gcc-c++,nsisb} bison flex make gcc-c++ ;;
            esac 1>/dev/null
        fi
        # shellcheck --external-sources "${0}"
        # shfmt -ci -fn -i 4 -d "${0}"
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
    --pkg={gio-2.0,lib{soup-3.0,archive}}
)
valac "${VAR[@]}" "${0%/*}/main.vala"

./main --help
./main
