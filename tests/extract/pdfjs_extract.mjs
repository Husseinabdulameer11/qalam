// Extract text the way Firefox's viewer builds its selectable text layer.
import { getDocument } from "pdfjs-dist/legacy/build/pdf.mjs";
const doc = await getDocument({ url: process.argv[2], verbosity: 0 }).promise;
let out = "";
for (let i = 1; i <= doc.numPages; i++) {
  const page = await doc.getPage(i);
  const tc = await page.getTextContent();
  for (const it of tc.items) { out += it.str; if (it.hasEOL) out += "\n"; }
  out += "\n";
}
process.stdout.write(out);
