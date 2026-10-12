// Job sets for the parity test: fonts from the repo, intervals as in sd-fonts.yaml.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// In the repo (tools/ftwasm/test) the checkout is three levels up; elsewhere set REPO.
const inRepo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
export const REPO = process.env.REPO ?? inRepo;
if (!fs.existsSync(path.join(REPO, 'lib/EpdFont/scripts'))) {
  throw new Error(`no firmware checkout at ${REPO}: run from the repository, or set REPO`);
}
const D = path.join(REPO, 'lib/EpdFont/scripts/downloaded_fonts');
// CFF-flavoured OpenType (no such font in the repo): Adobe's own OTF release of Source Serif 4.
const OTF = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../dl/fonts-otf');
const I = path.join(REPO, 'lib/EpdFont/scripts/instanced_fonts');

const LGC = 'builtin,latin-cyrillic,greek-letters,musical-symbols';
const LC = 'builtin,latin-cyrillic,musical-symbols';
const STYLES = ['regular', 'bold', 'italic', 'bolditalic'];
const STYLE_ID = { regular: 0, bold: 1, italic: 2, bolditalic: 3 };

const FAMILIES = {
  Literata: { intervals: LGC, files: {
    regular: `${D}/Literata/Literata-Regular.ttf`, bold: `${D}/Literata/Literata-Bold.ttf`,
    italic: `${D}/Literata/Literata-Italic.ttf`, bolditalic: `${D}/Literata/Literata-BoldItalic.ttf` } },
  SourceSerif4: { intervals: LGC, files: {
    regular: `${D}/SourceSerif4/SourceSerif4-Regular.ttf`, bold: `${D}/SourceSerif4/SourceSerif4-Bold.ttf`,
    italic: `${D}/SourceSerif4/SourceSerif4-It.ttf`, bolditalic: `${D}/SourceSerif4/SourceSerif4-BoldIt.ttf` } },
  Bitter: { intervals: LC, files: {
    regular: `${I}/Bitter/regular_wght500_1786820097.ttf`, bold: `${I}/Bitter/bold_wght700_1786820097.ttf`,
    italic: `${I}/Bitter/italic_wght500_1786820099.ttf`, bolditalic: `${I}/Bitter/bolditalic_wght700_1786820099.ttf` } },
  NotoSerif: { intervals: LGC, files: {
    regular: `${D}/NotoSerifExtended/NotoSerif-Regular.ttf`, bold: `${D}/NotoSerifExtended/NotoSerif-Bold.ttf`,
    italic: `${D}/NotoSerifExtended/NotoSerif-Italic.ttf`, bolditalic: `${D}/NotoSerifExtended/NotoSerif-BoldItalic.ttf` } },
  Inter: { intervals: LGC, files: {
    regular: `${I}/Inter/regular_opsz14_wght400_1786820755.ttf`, bold: `${I}/Inter/bold_opsz14_wght700_1786820755.ttf`,
    italic: `${I}/Inter/italic_opsz14_wght400_1786820757.ttf`, bolditalic: `${I}/Inter/bolditalic_opsz14_wght700_1786820757.ttf` } },
  SourceSerif4Otf: { intervals: LGC, files: {
    regular: `${OTF}/SourceSerif4-Regular.otf`, italic: `${OTF}/SourceSerif4-It.otf` } },
  Charis: { intervals: LGC, files: {
    regular: `${D}/Charis/NV_Charis-Regular.ttf`, bold: `${D}/Charis/NV_Charis-Bold.ttf`,
    italic: `${D}/Charis/NV_Charis-Italic.ttf`, bolditalic: `${D}/Charis/NV_Charis-BoldItalic.ttf` } },
};

function family(name, sizes, styles = STYLES) {
  const f = FAMILIES[name];
  const jobs = [];
  for (const size of sizes) {
    for (const style of styles) {
      jobs.push({ id: `${name}-${style}-${size}`, family: name, style: STYLE_ID[style], size,
                  font: f.files[style], intervals: f.intervals });
    }
  }
  return jobs;
}

export const JOB_SETS = {
  // The original measurement: Literata 14, all four styles; plus one other family.
  core: [...family('Literata', [14]), ...family('SourceSerif4', [14])],
  // A whole family at every shipped size (also the timing run).
  literata: family('Literata', [10, 12, 14, 16, 18]),
  // Breadth: more families, all styles, two sizes.
  wide: [...family('Bitter', [12, 16]), ...family('NotoSerif', [12, 16]),
         ...family('Inter', [12, 16]), ...family('Charis', [12, 16])],
  // CFF outlines (cubic curves, cff driver) instead of glyf.
  otf: family('SourceSerif4Otf', [12, 14, 18], ['regular', 'italic']),
};

export function jobsFor(names) {
  const seen = new Set();
  const out = [];
  for (const n of names) {
    if (!JOB_SETS[n]) throw new Error(`unknown job set ${n}; have ${Object.keys(JOB_SETS).join(', ')}`);
    for (const j of JOB_SETS[n]) if (!seen.has(j.id)) { seen.add(j.id); out.push(j); }
  }
  return out;
}

/**
 * The FreeType build to test: an explicit --dist, else dist/ (the layout committed
 * in the repo: dist/ftwasm.mjs), else dist/ft2.13.3-hblean/ (build.sh's default OUT).
 */
export function findDist(root, explicit) {
  const candidates = explicit ? [path.resolve(root, explicit)]
                              : [path.join(root, 'dist'), path.join(root, 'dist/ft2.13.3-hblean')];
  const d = candidates.find((c) => fs.existsSync(path.join(c, 'ftwasm.mjs')));
  if (!d) throw new Error(`no ftwasm.mjs in ${candidates.join(' or ')}; run build.sh`);
  return d;
}
