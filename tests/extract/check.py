"""Extract text from a PDF with five different engines and check it against the source.

  python3 check.py source.txt file.pdf

For each engine it reports:
  words  - how many source words can be found by searching the extracted text
  exact  - whether every source paragraph appears verbatim (whitespace-normalised)
"""
import re, subprocess, sys, os, unicodedata

BIDI_CTRL = dict.fromkeys(map(ord, "‎‏‪‫‬‭‮⁦⁧⁨⁩"))
HERE = os.path.dirname(os.path.abspath(__file__))

def norm(s):
    s = s.translate(BIDI_CTRL)
    return re.sub(r"\s+", " ", s).strip()

def source_paragraphs(path):
    paras, cur = [], []
    for line in open(path, encoding="utf-8"):
        t = line.strip()
        if not t or t.startswith("@"):
            if cur: paras.append(" ".join(cur)); cur = []
            continue
        if t.startswith("#"):
            if cur: paras.append(" ".join(cur)); cur = []
            paras.append(t[1:].strip()); continue
        cur.append(t)
    if cur: paras.append(" ".join(cur))
    return paras

def poppler(pdf):     return subprocess.run(["pdftotext", "-q", pdf, "-"], capture_output=True, text=True).stdout
def poppler_raw(pdf): return subprocess.run(["pdftotext", "-q", "-raw", pdf, "-"], capture_output=True, text=True).stdout
def pdfminer(pdf):
    from pdfminer.high_level import extract_text
    return extract_text(pdf)
def pypdf_(pdf):
    import pypdf
    return "\n".join(p.extract_text() for p in pypdf.PdfReader(pdf).pages)
def pdfium(pdf):
    import pypdfium2 as pdfium
    doc = pdfium.PdfDocument(pdf)
    return "\n".join(doc[i].get_textpage().get_text_range() for i in range(len(doc)))
def pdfjs(pdf):
    return subprocess.run(["node", os.path.join(HERE, "pdfjs_extract.mjs"), pdf],
                          capture_output=True, text=True, cwd=HERE).stdout

ENGINES = [("pdfium (Chrome, Edge)", pdfium), ("pdf.js (Firefox)", pdfjs),
           ("poppler pdftotext", poppler), ("poppler -raw", poppler_raw),
           ("pdfminer", pdfminer), ("pypdf", pypdf_)]

def main():
    src, pdf = sys.argv[1], os.path.abspath(sys.argv[2])
    paras = [norm(p) for p in source_paragraphs(src)]
    words = [w for p in paras for w in p.split()]
    verbose = "-v" in sys.argv
    print(f"{pdf}: {len(paras)} paragraphs, {len(words)} words")
    for name, fn in ENGINES:
        try: text = fn(pdf)
        except Exception as e: print(f"  {name:24} ERROR {e}"); continue
        flat = norm(text)
        found = sum(1 for w in words if w in flat)
        exact = sum(1 for p in paras if p in flat)
        print(f"  {name:24} words {found:3}/{len(words)}   paragraphs exact {exact}/{len(paras)}")
        if verbose: print("    " + flat[:300])

main()
