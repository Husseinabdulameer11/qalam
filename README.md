# qalam

An Arabic-first document compiler that writes PDFs whose text you can **copy and search**.

Copying text out of most Arabic PDFs gives garbled, unsearchable text. qalam makes PDFs where it doesn't, with what you need for a real document: several fonts, a table of contents, citations, figures, and tables.

`examples/coffee/coffee.pdf` is a 7-page article about Arabic coffee made with it.

## Why Arabic PDFs break

A PDF stores **glyphs at positions**, not text. Arabic goes through two transformations before it is drawn:

1. **Shaping.** Each letter becomes an initial, medial, final or isolated form; لا becomes one ligature; harakat become separate, shifted mark glyphs; some fonts (such as Amiri) also insert kashida pieces.
2. **Bidi.** The logical order you type becomes visual order, with English and numbers embedded left to right.

A text extractor has to undo both from the glyphs alone, using the font's `ToUnicode` map and its own guesses. Most producers write a poor map, and that is where the garbage comes from.

## What qalam does

| | |
|---|---|
| Shaping and bidi | HarfBuzz, FriBidi (UAX #9, with per-line rule L2) |
| ToUnicode | Built from the shaping result, not the font's cmap. A contextual form maps to its base letter and لا to ل + ا. Everything is in visual order, the order the glyphs are drawn in, because that is what extractors expect and reverse. |
| Harakat | A haraka is drawn as a shape, and its letter's glyph carries the text of both ("بَ"). As separate characters, extractors split them off with a space or put them on the wrong letter. |
| CIDs | Each (glyph, text, width) combination gets its own CID through a `CIDToGIDMap`. One glyph can mean different text in different places (a mirrored bracket, for example). Each occurrence also gets the exact width it is set at, so a line is one continuous `TJ` with no positioning jumps that extractors would read as spaces. |
| Pieces with no text | A glyph such as an inserted kashida is drawn as a vector outline, so it isn't text and no extractor sees a stray character. |
| Font | Each font is embedded as a subset (`hb-subset`, glyph ids kept), Type0/CIDFontType2. |
| ActualText | Off by default; `--actualtext=line` wraps each line in `/Span <</ActualText (logical text)>> BDC … EMC`. See below for why. |

## Copy and search results

These are the words of the source text you can find by searching each extractor's output (pdfium 159 through pypdfium2 5.15, pdf.js 4, poppler 24.02, pypdf 7.1).

**Coffee article** (577 words, 7 pages, justified text, six fonts):

| | pdfium (Chrome, Edge) | pdf.js (Firefox) | poppler | pypdf |
|---|---|---|---|---|
| qalam (default) | **568** | **545** | **515** | **523** |
| qalam `--actualtext=line` | 69 | **545** | 80 | **523** |

**Comparison with other producers** (same short sample, 81 words, `bench/run.sh`):

| Producer | pdfium | pdf.js | poppler | pdfminer | pypdf |
|---|---|---|---|---|---|
| XeLaTeX (polyglossia) | 25 | 24 | 23 | 23 | 22 |
| LuaLaTeX (babel, HarfBuzz) | 27 | 49 | 44 | **54** | 25 |
| Chrome print to PDF | 70 | 68 | 63 | 26 | **71** |
| LibreOffice | 70 | 67 | 64 | 25 | 66 |
| **qalam** | **74** | **74** | **74** | 26 | 67 |

The "pdfium" column is pdfium's text API through `pypdfium2`. It's the same engine Chrome uses, but this isn't a test of Chrome's copy menu. Acrobat and macOS Preview were not tested.

### Things learned the hard way

- **pdfium changed in 2026.** pdfium 153 ignored our visual-order text and needed ActualText (qalam with ActualText: 563/577 words; without: 459). pdfium 156 and later read visual-order text correctly on their own and reverse the letters inside ActualText (with: 69; without: 568). Recent Chrome has the newer pdfium, so ActualText is now off by default. `--actualtext=line` is there for older Chrome builds.
- **poppler (Evince, Okular, `pdftotext`) and pdfminer reverse ActualText too.** They treat everything as visual order.
- **`/ViewerPreferences /Direction /R2L` broke the older pdfium.** It made it reverse the already-correct ActualText of every line. qalam doesn't write it.
- **pdf.js reads any rightward jump inside a word as a space,** and it treats marks as zero-width whatever the PDF says. Giving each glyph its exact width fixed most of this.

### Known gaps

- Brackets and «» around Latin text inside an Arabic line can come out facing the wrong way (`(qalam (`); each extractor resolves those with its own bidi.
- pdfminer reverses all Arabic text, from every producer.
- Only TrueType (`glyf`) fonts are supported; CFF/OTF fonts such as Lemonada are not.
- There is no font fallback, and font changes happen per block, not inside a paragraph.
- Justification stretches spaces only; there is no kashida justification.
- A table that runs past a page doesn't repeat its header.

## Install

**Windows:** download `qalam-windows-setup.exe` from the [releases](https://github.com/Husseinabdulameer11/qalam/releases) and run it. It installs qalam with its fonts and examples and adds it to PATH, so open a new terminal and run `qalam --help`. By default it installs for the current user only, with no admin prompt.

**macOS / Linux:**
```
curl -fsSL https://raw.githubusercontent.com/Husseinabdulameer11/qalam/main/install.sh | sh
```
This installs into `~/.local` (set `QALAM_PREFIX` to change it, `... | sh -s -- --uninstall` to remove it) and checks the download against `SHA256SUMS`. Linux builds are static (x86_64 and aarch64); the macOS build is universal (Intel and Apple silicon).

**Docker** (any OS), in the folder with your `.qlm` files:
```
docker run --rm -v "${PWD}:/work" ghcr.io/husseinabdulameer11/qalam doc.qlm
```

**Codespaces:** open the repository in GitHub Codespaces and qalam is built, tested and on PATH when it opens.

Then:
```
qalam doc.qlm                 writes doc.pdf
qalam -o out.pdf doc.qlm
qalam --check doc.qlm         read and lay out only
qalam --fonts                 where the bundled fonts are
```

## Build from source

qalam's input format is a [gram](https://github.com/Husseinabdulameer11/gram) grammar, `src/qalam.gram`, so building it needs gram. Do one of these:

```
cd gram && make install PREFIX=~/.local     # then `make` here finds gramgen on PATH
git clone .../gram ../gram                  # a checkout next to qalam is found automatically
make GRAM=path/to/gram                      # or point at one
```

It also needs HarfBuzz (with hb-subset), FriBidi and zlib. Either use the system's:

```
sudo apt install libharfbuzz-dev libfribidi-dev zlib1g-dev        # Debian, Ubuntu
brew install harfbuzz fribidi                                     # macOS
make && make test
```

or let `tools/deps.sh` fetch pinned sources and compile them in, which needs nothing but a C/C++ compiler. Release builds do it this way:

```
tools/deps.sh
make LDFLAGS=-static          # one self-contained binary (Linux, Windows/MSYS2)
make install PREFIX=~/.local  # bin/qalam and share/qalam/fonts
```

On Windows, build in an MSYS2 UCRT64 shell (`pacman -S make curl tar xz mingw-w64-ucrt-x86_64-gcc`). `packaging/windows/qalam.iss` builds the installer with Inno Setup, and `RELEASING.md` explains how a tag push builds all of the above.

## Input format

```
% comment
@font body    Amiri-Regular.ttf           required; also: heading title sans sansbold quote ref
@size 13                                    body size in pt

@bib key = Author (Year). Title. Publisher.  anywhere in the file
@title …   @subtitle …   @author …   @date …
@maketitle                                  title page
@toc                                        table of contents page (filled in at the end)

# Section                                   numbered ١, ٢, …   (#* = unnumbered)
## Subsection                               numbered ١٫١ …
Paragraph text with a citation [@key].      becomes [n] and links to the reference
> A quote
- A list item

@figure 470 150                             width height in pt, origin top-left, y down
rect x y w h radius=8 fill=#e3efef stroke=accent width=1.2
circle cx cy r fill=…      ellipse cx cy rx ry …
line x1 y1 x2 y2 arrow=end|start|both stroke=… width=… dash=3
path "M 10 10 C 20 0, 40 0, 50 10 L 60 40 Z" fill=… stroke=…   (SVG commands M L H V C Q Z, lower case relative)
text x y "نص" font=sansbold size=11 color=grey anchor=left|center|right
@end
@caption …                                  "شكل n:" or "جدول n:" (numbered automatically)

@table 1,3                                  relative column widths; first column is on the right
! header | header
cell | cell
@end

@pagebreak
@bibliography                               numbered reference list, in citation order
```

Colours can be `#rrggbb`, `accent`, `ink`, `grey`, `white` or `none`. Headings and the table of contents become PDF bookmarks and links, and every page after the title page gets a running header and a page number.

The format is defined by `src/qalam.gram`. gram parses the whole file into a tree before anything is laid out, so a mistake is reported with its line and column, for example `doc.qlm:12:1: syntax error: expected newline, comment, shape or at_end` (a figure without `@end`) or `doc.qlm:3:1: unknown directive @tabel`. A citation to a missing `@bib` entry is a warning and prints as `[?]`.

## Options

```
-o FILE                  write the PDF to FILE (default: the input with .pdf)
--check                  read and lay out the document, write nothing
--actualtext=line|word   also wrap each line (or word) in ActualText (helps older pdfium only)
--no-tounicode           omit ToUnicode (reproduces a broken PDF, for testing)
--fonts                  print where the bundled fonts are
--version, --help
```

Fonts are named by file name in `@font` (`@font body Amiri-Regular.ttf`). qalam looks next to the input, in `QALAM_FONTS`, in the fonts it ships with (`fonts/`: Amiri, Noto Kufi/Naskh Arabic, Vazirmatn, all SIL OFL), and in the system font folders.

## Testing

```
make test                       # examples, syntax and other error messages, installed qalam finds its fonts,
                                # and (with pip install pypdfium2 pypdf) copy-and-search
pip install pdfminer.six && (cd tests/extract && npm install)
python3 tests/extract/check.py examples/sample.qlm out.pdf -v   # all five extractors
bench/run.sh                                                     # rebuilds the comparison table
```

`examples/coffee/figures.py` generates the figure blocks (geometry is easier in Python) and writes `coffee.qlm` from `coffee.src.qlm`.
