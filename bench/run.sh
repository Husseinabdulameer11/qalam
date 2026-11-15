#!/bin/sh
# Build the same sample with several PDF producers and measure how well
# five text extractors get the Arabic back. Needs: xelatex, lualatex
# (texlive-lang-arabic), google-chrome, soffice, the Amiri font, and the
# Python/Node deps listed in ../README.md.
set -e
cd "$(dirname "$0")"
xelatex  -interaction=nonstopmode sample-xelatex.tex  >/dev/null
lualatex -interaction=nonstopmode sample-lualatex.tex >/dev/null
google-chrome --headless --no-sandbox --disable-gpu --no-pdf-header-footer \
    --print-to-pdf=sample-chrome.pdf sample.html >/dev/null 2>&1
soffice --headless --convert-to pdf:writer_web_pdf_Export --outdir lo sample.html >/dev/null 2>&1
mv lo/sample.pdf sample-libreoffice.pdf && rmdir lo
../build/qalam -o sample-qalam.pdf ../examples/sample.qlm
../build/qalam --actualtext=line -o sample-qalam-at.pdf ../examples/sample.qlm
python3 table.py
