# qalam - Arabic-first documents, as PDFs you can copy and search.
# Works with GNU make on Linux, macOS and Windows (MSYS2).
#
# qalam's input format is a gram grammar (src/qalam.gram), so it is built
# with gram, the parser library: https://github.com/Husseinabdulameer11/gram
# Get gram and do ONE of these:
#
#   1. install it:        cd gram && make install          (or PREFIX=~/.local)
#                         then just `make` here; gramgen is found on PATH
#   2. keep a checkout next to this one, as ../gram         (found automatically)
#   3. point at one:      make GRAM=path/to/gram
#                    or:  make GRAM_PREFIX=/where/gram/is/installed
#
# It also needs HarfBuzz (with hb-subset), FriBidi and zlib. Either:
#
#   1. use the system's:   apt install libharfbuzz-dev libfribidi-dev zlib1g-dev
#                          brew install harfbuzz fribidi
#                          pacman -S mingw-w64-ucrt-x86_64-{harfbuzz,fribidi,zlib}
#      then just `make`    (found with pkg-config)
#
#   2. or build them in:   tools/deps.sh && make
#      (pinned sources in deps/, compiled into qalam; no system packages,
#       and with LDFLAGS=-static one self-contained binary. Releases do this.)
#
# make VENDOR=1 / VENDOR=0 forces one or the other.

CC       ?= cc
CXX      ?= c++
CFLAGS   ?= -O2
LDFLAGS  ?=
VERSION  ?= dev
PREFIX   ?= /usr/local
PYTHON   ?= python3

ifeq ($(OS),Windows_NT)
  EXE := .exe
  ifeq ($(origin CC),default)
    CC := gcc
  endif
  ifeq ($(origin CXX),default)
    CXX := g++
  endif
else
  EXE :=
endif

QALAM = build/qalam$(EXE)

# ---- find gram --------------------------------------------------------------------
ifeq ($(GRAM)$(GRAM_PREFIX),)
  GRAMGEN_ON_PATH := $(shell command -v gramgen 2>/dev/null)
  ifneq ($(GRAMGEN_ON_PATH),)
    # .../bin/gramgen -> ...
    GRAM_PREFIX := $(patsubst %/,%,$(dir $(patsubst %/,%,$(dir $(GRAMGEN_ON_PATH)))))
  else ifneq ($(wildcard ../gram/gram.c),)
    GRAM := ../gram
  endif
endif

ifneq ($(GRAM),)
  # a gram source checkout: build its library and gramgen into build/
  GRAMGEN  = build/gramgen$(EXE)
  GRAM_LIB = build/gram.o
  GRAM_INC = -I$(GRAM)
  GRAM_FROM = source checkout $(GRAM)
else ifneq ($(GRAM_PREFIX),)
  # an installed gram (make install)
  GRAMGEN  = $(GRAM_PREFIX)/bin/gramgen$(EXE)
  GRAM_LIB = $(GRAM_PREFIX)/lib/libgram.a
  GRAM_INC = -I$(GRAM_PREFIX)/include
  GRAM_FROM = installed in $(GRAM_PREFIX)
else
  GRAM_MISSING = 1
endif

# ---- where the libraries come from -------------------------------------------
ifeq ($(VENDOR),)
  ifneq ($(wildcard deps/harfbuzz/.version),)
    VENDOR := 1
  else ifeq ($(shell pkg-config --exists harfbuzz harfbuzz-subset fribidi zlib 2>/dev/null && echo yes),yes)
    VENDOR := 0
  else
    VENDOR := missing
  endif
endif

ifeq ($(VENDOR),1)
  DEP_INC  = -Ideps/harfbuzz/src -Ideps/fribidi/lib -Ideps/zlib -DFRIBIDI_LIB_STATIC
  HB_OBJ   = build/deps/harfbuzz.o build/deps/harfbuzz-subset.o
  FB_OBJ   = $(patsubst deps/fribidi/lib/%.c,build/deps/fribidi/%.o,$(wildcard deps/fribidi/lib/fribidi*.c))
  ZLIB_OBJ = $(patsubst %,build/deps/zlib/%.o,adler32 compress crc32 deflate inffast inflate inftrees trees zutil)
  DEP_OBJ  = $(HB_OBJ) $(FB_OBJ) $(ZLIB_OBJ)
  DEP_LIBS =
  LINK     = $(CXX)
  DEPS_FROM = built in from deps/
else ifeq ($(VENDOR),0)
  DEP_INC  = $(shell pkg-config --cflags harfbuzz harfbuzz-subset fribidi zlib)
  DEP_LIBS = $(shell pkg-config --libs harfbuzz harfbuzz-subset fribidi zlib)
  DEP_OBJ  =
  LINK     = $(CC)
  DEPS_FROM = system (pkg-config)
endif

all: $(QALAM)

need-gram:
ifdef GRAM_MISSING
	@echo "qalam: gram was not found. qalam's input format is a gram grammar."
	@echo "       Get gram (https://github.com/Husseinabdulameer11/gram), then either run"
	@echo "       'make install' in it, put it next to this folder as ../gram, or run:"
	@echo "       make GRAM=path/to/gram"
	@exit 1
endif

ifneq ($(GRAM),)
build/gram.o: $(GRAM)/gram.c $(GRAM)/gram.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $(GRAM)/gram.c -o $@

# gramgen runs on the build machine, so it is built with the host compiler
HOSTCC ?= $(CC)
$(GRAMGEN): $(GRAM)/tools/gramgen.c $(GRAM)/gram.c $(GRAM)/gram.h
	@mkdir -p build
	$(HOSTCC) -O2 -I$(GRAM) $(GRAM)/tools/gramgen.c $(GRAM)/gram.c -o $@
endif

# grammar -> rule ids and the grammar text (build/qalam_grammar.h)
build/qalam_grammar.h: src/qalam.gram $(GRAMGEN) | need-gram
	@mkdir -p build
	$(GRAMGEN) src/qalam.gram qalam build

need-deps:
ifeq ($(VENDOR),missing)
	@echo "qalam needs HarfBuzz, FriBidi and zlib, and found neither system ones (pkg-config)"
	@echo "nor deps/. Either install them (see the top of this Makefile) or run:"
	@echo "    tools/deps.sh && make"
	@exit 1
endif

$(QALAM): src/qalam.c build/qalam_grammar.h $(GRAM_LIB) $(DEP_OBJ) | need-gram need-deps
	@mkdir -p build
	$(CC) $(CFLAGS) -Wall -Wextra -Wno-format-truncation -DQALAM_VERSION='"$(VERSION)"' -Ibuild $(GRAM_INC) $(DEP_INC) \
	    -c src/qalam.c -o build/qalam.o
	$(LINK) $(CFLAGS) build/qalam.o $(GRAM_LIB) $(DEP_OBJ) $(DEP_LIBS) $(LDFLAGS) -lm -o $@
	@echo "qalam: built $@ (gram: $(GRAM_FROM); libraries: $(DEPS_FROM))"

# ---- the vendored libraries ----------------------------------------------------
# HarfBuzz as its two "amalgamated" sources; no threads needed (HB_NO_MT).
HB_FLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -DHB_NO_MT -w

build/deps/harfbuzz.o: deps/harfbuzz/src/harfbuzz.cc
	@mkdir -p $(dir $@)
	$(CXX) $(HB_FLAGS) -c $< -o $@
build/deps/harfbuzz-subset.o: deps/harfbuzz/src/qalam-hb-subset.cc
	@mkdir -p $(dir $@)
	$(CXX) $(HB_FLAGS) -c $< -o $@
build/deps/fribidi/%.o: deps/fribidi/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -DFRIBIDI_LIB_STATIC -DFRIBIDI_BUILD -DHAVE_STRINGIZE -DHAVE_STDLIB_H -DHAVE_STRING_H \
	    -DHAVE_FRIBIDI_UNICODE_VERSION_H -Ideps/fribidi/lib -c $< -o $@
build/deps/zlib/%.o: deps/zlib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -Ideps/zlib -c $< -o $@

deps:
	sh tools/deps.sh

# ---- tests -------------------------------------------------------------------------
# Every example builds to a well-formed PDF, every error case fails with its
# message, the installed qalam run from PATH finds its fonts, and (if Python
# with pypdfium2 + pypdf is there) the text copies back out of the PDFs.
test: $(QALAM)
	@mkdir -p build/test
	@$(QALAM) --version
	@for f in $$(find examples -name '*.qlm' ! -name '*.src.qlm'); do \
	    out=build/test/$$(basename $$f .qlm).pdf; \
	    $(QALAM) -o $$out $$f 2>build/err.txt || { cat build/err.txt; exit 1; }; \
	    head -c 8 $$out | grep -q '%PDF-1.7' || { echo "$$f: $$out is not a PDF"; exit 1; }; \
	    tail -c 8 $$out | grep -q '%%EOF' || { echo "$$f: $$out is cut off"; exit 1; }; \
	done
	@for f in tests/errors/*.qlm; do \
	    if $(QALAM) --check $$f 2>build/err.txt; then echo "$$f: expected an error"; exit 1; fi; \
	    grep -qF "$$(sed -n 's/^% expect: //p' $$f)" build/err.txt || { echo "$$f: wrong error:"; cat build/err.txt; exit 1; }; \
	done
	@if $(PYTHON) -c "import pypdfium2, pypdf" 2>/dev/null; then \
	    $(PYTHON) tests/copytest.py build/test || exit 1; \
	 else echo "qalam: no python3 with pypdfium2 + pypdf, skipping the copy-and-search test"; fi
	@# installed and run from PATH (argv[0] is then just "qalam"), it must still
	@# find the fonts it ships with in ../share/qalam/fonts
	@rm -rf build/prefix
	@$(MAKE) --no-print-directory install DESTDIR= PREFIX="$(CURDIR)/build/prefix" >/dev/null
	@cd tests/install && PATH="$(CURDIR)/build/prefix/bin:$$PATH" qalam$(EXE) --check uses_fonts.qlm >../../build/err.txt 2>&1 \
	    || { cat ../../build/err.txt; echo "qalam: installed qalam, run from PATH, did not find its fonts"; exit 1; }
	@echo "qalam: all tests passed"

# examples/coffee/coffee.qlm is generated (figure geometry is easier in Python)
coffee:
	cd examples/coffee && $(PYTHON) figures.py

# ---- install (Linux, macOS, MSYS2) -----------------------------------------------
# $(PREFIX)/bin/qalam, and the fonts in $(PREFIX)/share/qalam/fonts where it looks
install: $(QALAM)
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/qalam/fonts
	cp $(QALAM) $(DESTDIR)$(PREFIX)/bin/
	cp fonts/*.ttf fonts/LICENSE-*.txt fonts/README.md $(DESTDIR)$(PREFIX)/share/qalam/fonts/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/qalam$(EXE)
	rm -rf $(DESTDIR)$(PREFIX)/share/qalam

clean:
	rm -rf build dist

distclean: clean
	rm -rf deps

.PHONY: all test install uninstall clean distclean deps coffee need-deps need-gram
