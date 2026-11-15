# Fonts that come with qalam

All of these are under the **SIL Open Font License 1.1** (see the `LICENSE-*.txt`
next to them), which allows bundling and embedding them in PDFs.

| File | Family | Use in the examples | Source |
|---|---|---|---|
| Amiri-Regular.ttf, Amiri-Bold.ttf | Amiri (Naskh) | body text, references | https://github.com/aliftype/amiri |
| NotoKufiArabic-Bold.ttf | Noto Kufi Arabic | headings | https://github.com/notofonts/arabic |
| NotoNaskhArabic-Regular.ttf | Noto Naskh Arabic | quotes | https://github.com/notofonts/arabic |
| Vazirmatn-Regular.ttf, -Bold, -Black | Vazirmatn | titles, figures, tables | https://github.com/rastikerdar/vazirmatn |

A document names a font by file name (`@font body Amiri-Regular.ttf`). qalam looks for it:

1. as written (an absolute path, or relative to the current folder)
2. next to the input file
3. in each folder in `QALAM_FONTS` (`:`-separated, `;` on Windows)
4. here, the fonts that come with qalam (`qalam --fonts` prints the folder)
5. in the system font folders

Any TrueType font with Arabic works. CFF-based `.otf` fonts are not supported yet.
