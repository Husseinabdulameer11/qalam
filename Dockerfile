# qalam in a container: a small Alpine image with a static `qalam` binary and
# the fonts it ships with.
#
#   docker run --rm -v "${PWD}:/work" ghcr.io/husseinabdulameer11/qalam doc.qlm
#   docker run --rm -v "${PWD}:/work" ghcr.io/husseinabdulameer11/qalam -o out.pdf doc.qlm
#
# Files are read from and written to the folder you mount at /work.
#
# Building it yourself needs gram. Either hand Docker your gram folder:
#
#   docker build --build-context gram=../gram -t qalam .
#
# or let it clone gram from git:
#
#   docker build --build-arg GRAM_REPO=https://github.com/Husseinabdulameer11/gram.git -t qalam .
#
# HarfBuzz, FriBidi and zlib are downloaded by tools/deps.sh, pinned by
# checksum, and compiled in. A deps/ folder from an earlier run is reused.

ARG ALPINE=alpine:3.20

# ---- gram, when no `--build-context gram=...` is given: clone it ----------------
FROM ${ALPINE} AS gram-git
ARG GRAM_REPO=
ARG GRAM_REF=main
RUN if [ -z "$GRAM_REPO" ]; then \
        echo "qalam needs gram: build with --build-context gram=path/to/gram"; \
        echo "or --build-arg GRAM_REPO=https://github.com/<owner>/gram.git"; \
        exit 1; \
    fi \
 && apk add --no-cache git \
 && git clone --depth 1 --branch "$GRAM_REF" "$GRAM_REPO" /gram

# A stage named `gram` holding just gram's files. `--build-context gram=...`
# replaces this whole stage with the given folder.
FROM scratch AS gram
COPY --from=gram-git /gram /

# ---- build a static qalam and run its tests -----------------------------------
FROM ${ALPINE} AS build
RUN apk add --no-cache build-base curl tar xz
COPY --from=gram / /src/gram
COPY . /src/qalam
WORKDIR /src/qalam
ARG VERSION=dev
RUN sh tools/deps.sh \
 && make clean \
 && make -j"$(nproc)" test GRAM=/src/gram VENDOR=1 VERSION="$VERSION" LDFLAGS=-static \
 && strip build/qalam

# ---- the image people run --------------------------------------------------------
# qalam is in /usr/local/bin and finds its fonts in /usr/local/share/qalam/fonts,
# next to it.
FROM ${ALPINE}
LABEL org.opencontainers.image.title="qalam" \
      org.opencontainers.image.description="Arabic-first documents, as PDFs you can copy and search" \
      org.opencontainers.image.licenses="MIT"
COPY --from=build /src/qalam/build/qalam /usr/local/bin/qalam
COPY --from=build /src/qalam/fonts /usr/local/share/qalam/fonts
COPY --from=build /src/qalam/examples /usr/local/share/qalam/examples
COPY --from=build /src/qalam/LICENSE /src/qalam/THIRD_PARTY.md /usr/local/share/qalam/
COPY --from=build /src/gram/LICENSE                /usr/local/share/qalam/licenses/gram.txt
COPY --from=build /src/qalam/deps/harfbuzz/COPYING /usr/local/share/qalam/licenses/HarfBuzz.txt
COPY --from=build /src/qalam/deps/fribidi/COPYING  /usr/local/share/qalam/licenses/FriBidi.txt
COPY --from=build /src/qalam/deps/zlib/LICENSE     /usr/local/share/qalam/licenses/zlib.txt
WORKDIR /work
ENTRYPOINT ["qalam"]
CMD ["--help"]
