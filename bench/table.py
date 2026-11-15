"""Print a markdown table: words found by searching each extractor's output."""
import os, sys
sys.argv = ["check", "../examples/sample.qlm", "x"]
BENCH = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(BENCH, "../tests/extract/check.py")).read().replace("main()\n", "")
src = src.replace("HERE = os.path.dirname(os.path.abspath(__file__))",
                  f"HERE = {os.path.join(BENCH, '../tests/extract')!r}")
exec(src)
paras = [norm(p) for p in source_paragraphs(os.path.join(BENCH, "../examples/sample.qlm"))]
words = [w for p in paras for w in p.split()]
producers = [("XeLaTeX (polyglossia)", "sample-xelatex.pdf"), ("LuaLaTeX (babel, HarfBuzz)", "sample-lualatex.pdf"),
             ("Chrome print to PDF", "sample-chrome.pdf"), ("LibreOffice", "sample-libreoffice.pdf"),
             ("**qalam** (default)", "sample-qalam.pdf"), ("**qalam** --actualtext=line", "sample-qalam-at.pdf")]
engines = [e for e in ENGINES if e[0] != "poppler -raw"]
print("| Producer | " + " | ".join(e[0] for e in engines) + " |")
print("|---|" + "---|" * len(engines))
for label, f in producers:
    cells = []
    for _, fn in engines:
        t = norm(fn(os.path.join(BENCH, f)))
        cells.append(f"{sum(w in t for w in words)}/{len(words)}")
    print(f"| {label} | " + " | ".join(cells) + " |")
