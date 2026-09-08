#!/usr/bin/env bash

function f_check_require() {
    declare -r required_tools=(
        valac
        meson
        make{,nsis}
        python
        perl
        ninja
        {y,n}asm
        gperf
        bison
        flex
        itstool
        sh{fmt,ellcheck}
    )
    for tool in "${required_tools[@]}"; do
        if ! command -v "$tool" >/dev/null; then
            printf "Missing dependency: %s\n" "${tool}"
            return 1
        fi
    done
    return 0
}

function f_deb
(
    declare -ra DEBIAN_PACKAGES=(
        valac
        clang-{tidy,format}
        sh{fmt,ellcheck}
        meson
        auto{conf,make}
        gettext
        {lib,its}tool
        cmake
        {y,n}asm
        gperf
        python3
        perl
        ninja-build
        pkgconf
        g++-mingw-w64-x86-64
        libgomp1
        gcc
        nsis
        bison
        flex
        make
        lib{archive,soup-3.0,gee-0.8,git2-glib-1.0}-dev
    )
    sudo bash -c '
        apt-get update
        apt-get install -y "${DEBIAN_PACKAGES[@]}" "${@}"
    '
)

function f_rpm
(
    declare -ra FEDORA_PACKAGES=(
        sh{fmt,sh}ellcheck
        meson
        auto{conf,make}
        {lib,its}tool
        gettext
        cmake
        {y,n}asm
        gperf
        python{,3-pip}
        perl
        ninja-build
        pkgconf
        mingw32-{libgomp,gcc,gcc-c++,nsisb}
        bison
        flex
        make
        gcc-c++
        lib{archive,git2-glib}-devel
    )
    sudo dnf install -y "${FEDORA_PACKAGES[@]}" "${@}"
)

function f_setup
{
    if [[ -f '/etc/os-release' ]]; then
        declare -ra PACKAGES=(
            sh{fmt,sh}ellcheck
            meson
            auto{conf,make}
            {lib,its}tool
            gettext
            {c,}make
            {y,n}asm
            gperf
            python{,3-pip}
            perl
            ninja-build
            pkgconf
            mingw32-{libgomp,gcc,gcc-c++,nsisb}
            bison
            flex
            gcc-c++
            lib{archive,git2-glib}-devel
        )
        source '/etc/os-release'
        if ! f_check_require; then
            case ${ID:?} in
                msys2) return 0 ;;
                debian | ubuntu) f_deb "${FEDORA_PACKAGES[@]}";;
                fedora | alma) f_rpm "${FEDORA_PACKAGES[@]}";;
                *)
                    echo "Unsupported distribution: $ID"
                    exit 1
                ;;
            esac 1>/dev/null
        fi
        f_check_require
        shellcheck --external-sources "${0}" #~  packages/*/*.sh
        shfmt -ci -fn -i 4 -d "${0}" #~  packages/*/*.sh
    fi
}

set -xeuo pipefail

if ((${#})); then
    case ${1} in
        setup) f_setup ;;
        build)
            declare -ar VAR=(
                --verbose
                --fatal-warnings
                --Xcc=-O3
                --cc="${CC:-clang}"
                --enable-{checking,mem-profiler,gobject-tracing}
                --pkg={gio-2.0,lib{soup-3.0,archive,git2-glib-1.0}}
            )
            vala  "${VAR[@]}" src/main.vala --run-args '--efl --insecure --verbose'
            ;;
    esac
fi
