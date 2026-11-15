#!/bin/sh
# Runs once when the dev container is created (postCreateCommand).
# Installs the libraries, gets gram and installs it into ~/.local, then builds,
# tests and installs qalam there too.
set -eu

GRAM_REPO=${GRAM_REPO:-https://github.com/Husseinabdulameer11/gram.git}
GRAM_REF=${GRAM_REF:-main}
PREFIX=$HOME/.local
export PATH="$PREFIX/bin:$PATH"

echo "== installing build tools and libraries"
sudo apt-get update -q
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
    build-essential pkg-config libharfbuzz-dev libfribidi-dev zlib1g-dev python3-pip
pip3 install --user --break-system-packages -q pypdfium2 pypdf ||
    echo "(could not install pypdfium2/pypdf; make test skips the copy-and-search test)"

# gram next to qalam if we may write there (Codespaces: /workspaces/gram)
qalam_dir=$(pwd)
parent=$(dirname "$qalam_dir")
if [ -w "$parent" ]; then gram_dir=$parent/gram; else gram_dir=$HOME/gram; fi

if [ -f "$gram_dir/gram.c" ]; then
    echo "== using gram in $gram_dir"
else
    echo "== cloning gram ($GRAM_REPO) into $gram_dir"
    git clone --depth 1 --branch "$GRAM_REF" "$GRAM_REPO" "$gram_dir"
fi

echo "== installing gram into $PREFIX"
make -C "$gram_dir" install PREFIX="$PREFIX"

echo "== building, testing and installing qalam"
make clean
make test GRAM_PREFIX="$PREFIX"
make install PREFIX="$PREFIX"

echo
echo "Ready. Try:  qalam examples/coffee/coffee.qlm"
echo "             and open examples/coffee/coffee.pdf"
echo "gram is in $gram_dir. After changing src/qalam.gram or src/qalam.c:  make install PREFIX=$PREFIX"
