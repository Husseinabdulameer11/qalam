# Third-party code and fonts

Release builds (the installer, the archives, the Docker image) compile these
libraries into qalam. gram comes from its own repository (see the Makefile);
the other three are downloaded by `tools/deps.sh`, pinned by version and
checksum. Their license texts ship in `licenses/` next to the program.

| Library | Version | License | Used for |
|---|---|---|---|
| [gram](https://github.com/Husseinabdulameer11/gram) | 0.1.0 | MIT | parsing `.qlm` files (`src/qalam.gram`) |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | 10.1.0 | MIT ("Old MIT") | shaping, font subsetting |
| [GNU FriBidi](https://github.com/fribidi/fribidi) | 1.0.16 | LGPL-2.1-or-later | the Unicode bidi algorithm |
| [zlib](https://github.com/madler/zlib) | 1.3.1 | zlib | compressing PDF streams |

FriBidi is LGPL. qalam's complete source and build scripts are public, so
anyone can rebuild qalam against a modified FriBidi (`tools/deps.sh`, edit
`deps/fribidi`, `make`), which is what the LGPL asks of a statically linked
program.

The fonts in `fonts/` are under the SIL Open Font License 1.1; see
`fonts/README.md` and the `fonts/LICENSE-*.txt` files.
