#!/bin/sh
# Install qalam on Linux or macOS from the latest GitHub release.
#
#   curl -fsSL https://raw.githubusercontent.com/Husseinabdulameer11/qalam/main/install.sh | sh
#
# Options, as environment variables:
#   QALAM_VERSION=0.1.0          install that release instead of the latest
#   QALAM_PREFIX=/some/prefix    where to install (default ~/.local)
#
#   ... | sh -s -- --uninstall  remove it again
#
# What it does: downloads qalam-<os>-<arch>.tar.gz and SHA256SUMS from the
# release, checks the checksum, and puts
#   qalam                in <prefix>/bin
#   the fonts            in <prefix>/share/qalam/fonts (qalam finds them there)
#   the examples         in <prefix>/share/qalam/examples
# Nothing outside your home folder is touched unless you set QALAM_PREFIX there.
set -eu

QALAM_REPO=${QALAM_REPO:-Husseinabdulameer11/qalam}

# Everything is inside main, so a download cut off half way runs nothing.
main() {
    prefix=${QALAM_PREFIX:-$HOME/.local}
    bindir=$prefix/bin
    sharedir=$prefix/share/qalam     # qalam looks for ../share/qalam/fonts

    if [ "${1:-}" = "--uninstall" ]; then
        rm -f "$bindir/qalam"
        rm -rf "$sharedir"
        say "removed $bindir/qalam and $sharedir"
        exit 0
    fi

    case $(uname -s) in
        Linux)  os=linux ;;
        Darwin) os=macos ;;
        MINGW*|MSYS*|CYGWIN*)
            die "on Windows, use the installer (qalam-windows-setup.exe) from the releases page" ;;
        *) die "no prebuilt qalam for $(uname -s); build it from source (see README)" ;;
    esac
    case $(uname -m) in
        x86_64|amd64)  arch=x86_64 ;;
        aarch64|arm64) arch=aarch64 ;;
        *) die "no prebuilt qalam for $(uname -m); build it from source (see README)" ;;
    esac
    [ "$os" = macos ] && arch=universal     # one binary for Intel and Apple silicon

    name=qalam-$os-$arch
    if [ -n "${QALAM_DOWNLOAD_BASE:-}" ]; then
        base=$QALAM_DOWNLOAD_BASE                         # mirrors and testing
    elif [ -n "${QALAM_VERSION:-}" ]; then
        base=https://github.com/$QALAM_REPO/releases/download/v${QALAM_VERSION#v}
    else
        base=https://github.com/$QALAM_REPO/releases/latest/download
    fi

    tmp=$(mktemp -d 2>/dev/null || mktemp -d -t qalam)
    trap 'rm -rf "$tmp"' EXIT INT TERM

    say "downloading $name.tar.gz"
    fetch "$base/$name.tar.gz" "$tmp/$name.tar.gz" ||
        die "download failed: $base/$name.tar.gz"

    if fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" 2>/dev/null; then
        want=$(grep " $name.tar.gz\$" "$tmp/SHA256SUMS" | cut -d' ' -f1)
        got=$(sha256 "$tmp/$name.tar.gz")
        if [ -z "$want" ]; then
            die "$name.tar.gz is not listed in SHA256SUMS"
        elif [ -z "$got" ]; then
            say "warning: no sha256sum or shasum found, checksum not verified"
        elif [ "$want" != "$got" ]; then
            die "checksum mismatch for $name.tar.gz (download corrupted?)"
        else
            say "checksum ok"
        fi
    else
        say "warning: no SHA256SUMS in this release, checksum not verified"
    fi

    tar -xzf "$tmp/$name.tar.gz" -C "$tmp"
    [ -f "$tmp/$name/bin/qalam" ] || die "the archive has no qalam binary"

    mkdir -p "$bindir" "$sharedir"
    rm -f "$bindir/qalam"
    cp "$tmp/$name/bin/qalam" "$bindir/qalam"
    chmod 755 "$bindir/qalam"
    rm -rf "$sharedir/fonts" "$sharedir/examples"
    cp -R "$tmp/$name/fonts" "$sharedir/fonts"
    cp -R "$tmp/$name/examples" "$sharedir/examples"
    cp "$tmp/$name/README.md" "$sharedir/README.md"
    if [ "$os" = macos ]; then
        xattr -d com.apple.quarantine "$bindir/qalam" 2>/dev/null || true
    fi

    say "installed $("$bindir/qalam" --version) to $bindir/qalam"
    say "examples are in $sharedir/examples"

    case ":$PATH:" in
        *":$bindir:"*)
            say ""
            say "try:  qalam -o coffee.pdf $sharedir/examples/coffee/coffee.qlm"
            say "      and open coffee.pdf" ;;
        *)
            say ""
            say "$bindir is not on your PATH yet. Add it with:"
            case ${SHELL:-} in
                */zsh)  say "  echo 'export PATH=\"$bindir:\$PATH\"' >> ~/.zshrc && exec zsh" ;;
                */fish) say "  fish_add_path $bindir" ;;
                *)      say "  echo 'export PATH=\"$bindir:\$PATH\"' >> ~/.bashrc && exec bash" ;;
            esac
            say "then:  qalam -o coffee.pdf $sharedir/examples/coffee/coffee.qlm" ;;
    esac
}

say() { printf '%s\n' "$*"; }
die() { printf 'qalam install: %s\n' "$*" >&2; exit 1; }

fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        die "needs curl or wget"
    fi
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

main "$@"
