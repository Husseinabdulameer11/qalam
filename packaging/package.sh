#!/bin/sh
# Pack a built qalam into a release archive.
#
#   packaging/package.sh <target>      e.g. linux-x86_64, macos-universal,
#                                           windows-x86_64
#
# Run from the qalam folder after `make`. Writes dist/qalam-<target>.tar.gz
# (dist/qalam-<target>.zip for windows-*) holding one folder:
#
#   qalam-<target>/bin/qalam[.exe]
#                  fonts/        the bundled fonts (qalam finds them at ../fonts)
#                  examples/     the examples, each with its PDF
#                  licenses/     licenses of gram, HarfBuzz, FriBidi, zlib
#                  README.md  LICENSE  THIRD_PARTY.md
#
# The archive names carry no version so that
# https://github.com/<owner>/qalam/releases/latest/download/<name> always
# points at the newest release (install.sh relies on that).
set -eu

target=${1:?usage: packaging/package.sh <target>}
name=qalam-$target
exe=build/qalam
case $target in windows-*) exe=build/qalam.exe ;; esac

[ -f "$exe" ] || { echo "package.sh: $exe not found, run make first" >&2; exit 1; }

rm -rf "dist/$name"
mkdir -p "dist/$name/bin" "dist/$name/fonts" "dist/$name/examples"
cp "$exe" "dist/$name/bin/"
cp README.md LICENSE THIRD_PARTY.md "dist/$name/"
# licenses of the libraries compiled in (when built from deps/)
mkdir -p "dist/$name/licenses"
for g in ${GRAM:-} gram-src ../gram; do
    if [ -f "$g/LICENSE" ]; then cp "$g/LICENSE" "dist/$name/licenses/gram.txt"; break; fi
done
if [ -d deps/harfbuzz ]; then
    cp deps/harfbuzz/COPYING "dist/$name/licenses/HarfBuzz.txt"
    cp deps/fribidi/COPYING  "dist/$name/licenses/FriBidi.txt"
    cp deps/zlib/LICENSE     "dist/$name/licenses/zlib.txt"
fi
cp fonts/*.ttf fonts/LICENSE-*.txt fonts/README.md "dist/$name/fonts/"
cp -R examples/. "dist/$name/examples/"
# a PDF next to every example, made by this very binary (skipped when it is
# for another CPU, e.g. packing the aarch64 build on an x86_64 machine)
if "$exe" --version >/dev/null 2>&1; then
    for f in $(cd "dist/$name/examples" && find . -name '*.qlm' ! -name '*.src.qlm'); do
        "$exe" -o "dist/$name/examples/${f%.qlm}.pdf" "dist/$name/examples/$f" 2>/dev/null
    done
fi

cd dist
case $target in
    windows-*) rm -f "$name.zip";    zip -qr "$name.zip" "$name";       echo "wrote dist/$name.zip" ;;
    *)         rm -f "$name.tar.gz"; tar -czf "$name.tar.gz" "$name";   echo "wrote dist/$name.tar.gz" ;;
esac
