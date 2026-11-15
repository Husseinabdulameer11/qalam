"""Copy-and-search test: the words of each example must come back out of its PDF.

    python3 tests/copytest.py build/test

Uses pdfium (the engine in Chrome and Edge) and pypdf. The thresholds are a
floor that the current output clears, so a change that breaks text
extraction fails the build.
"""
import os, re, sys
import pypdf, pypdfium2

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CHECKS = [  # source, pdf name, min pdfium share, min pypdf share
    ("examples/coffee/coffee.qlm", "coffee.pdf", 0.95, 0.85),
    ("examples/sample.qlm",        "sample.pdf", 0.88, 0.80),
]
BIDI = dict.fromkeys(map(ord, "‎‏‪‫‬‭‮⁦⁧⁨⁩"))
DRAW = ("rect", "circle", "ellipse", "line", "path", "text")

def norm(s):
    return re.sub(r"\s+", " ", s.translate(BIDI)).strip()

def source_words(path):
    text = open(path, encoding="utf-8").read()
    order = {}
    for k in re.findall(r"\[@(\w+)\]", text):
        order.setdefault(k, len(order) + 1)
    words, in_block = [], False
    for line in text.split("\n"):
        t = line.strip()
        if t.startswith(("@figure", "@table")): in_block = True; continue
        if t == "@end": in_block = False; continue
        if in_block or not t or t[0] in "@%": continue
        t = re.sub(r"\[@(\w+)\]", lambda m: f"[{order[m.group(1)]}]", t).lstrip("#*>- ").strip()
        words += t.split()
    return words

def main():
    outdir = sys.argv[1]
    ok = True
    for src, pdf, min_pdfium, min_pypdf in CHECKS:
        words = source_words(os.path.join(ROOT, src))
        path = os.path.join(outdir, pdf)
        doc = pypdfium2.PdfDocument(path)
        texts = {
            "pdfium": norm(" ".join(doc[i].get_textpage().get_text_range() for i in range(len(doc)))),
            "pypdf": norm(" ".join(p.extract_text() for p in pypdf.PdfReader(path).pages)),
        }
        for engine, floor in (("pdfium", min_pdfium), ("pypdf", min_pypdf)):
            found = sum(w in texts[engine] for w in words)
            share = found / len(words)
            status = "ok" if share >= floor else "FAILED"
            print(f"copytest: {pdf:12} {engine:7} {found}/{len(words)} words ({share:.0%}, needs {floor:.0%}) {status}")
            ok &= share >= floor
    sys.exit(0 if ok else 1)

main()
