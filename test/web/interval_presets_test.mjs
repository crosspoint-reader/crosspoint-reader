// The browser converter (tools/ftwasm/js/cpfont-raster.mjs) and fontconvert_sdcard.py offer the same
// interval presets and resolve a preset list to the same intervals, and latin-ext covers the symbols
// European text uses: the euro, trade mark and numero signs, and the minus sign.
//
//   node test/web/interval_presets_test.mjs [deps dir, unused]
//
// Needs python3 on the PATH, but not the converter's packages: the presets are read from the
// script's source (its top-level constants and resolve_intervals()), not by importing it.
import { spawnSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const { INTERVAL_PRESETS, resolveIntervals } = await import(join(repo, 'tools/ftwasm/js/cpfont-raster.mjs'));

let failures = 0;
const check = (ok, what) => {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
};

// Preset lists as sd-fonts.yaml and the Font Manager page use them, plus every preset on its own.
const lists = [
  'builtin,latin-cyrillic,greek-letters,musical-symbols',
  'builtin,latin-cyrillic,musical-symbols',
  'builtin,latin-cyrillic,greek-letters,georgian,armenian,ethiopic,musical-symbols',
  'builtin,latin-ext',
  'latin-ext,greek,cyrillic',
  ...Object.keys(INTERVAL_PRESETS),
];

const py = `
import ast, json, sys
path, lists = sys.argv[1], json.loads(sys.argv[2])
tree = ast.parse(open(path, encoding='utf-8').read(), path)
names = ('_LATIN_EXT', '_CYRILLIC', 'INTERVAL_PRESETS')
body = [n for n in tree.body
        if (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in names for t in n.targets))
        or (isinstance(n, ast.FunctionDef) and n.name == 'resolve_intervals')]
ns = {'sys': sys}
exec(compile(ast.Module(body=body, type_ignores=[]), path, 'exec'), ns)
print(json.dumps({'presets': ns['INTERVAL_PRESETS'],
                  'resolved': {s: ns['resolve_intervals'](s) for s in lists}}))
`;
const r = spawnSync('python3', ['-I', '-c', py, join(repo, 'lib/EpdFont/scripts/fontconvert_sdcard.py'),
                                JSON.stringify(lists)], { encoding: 'utf8' });
if (r.status !== 0) {
  console.error(r.stderr);
  process.exit(1);
}
const python = JSON.parse(r.stdout);

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
check(same(Object.keys(INTERVAL_PRESETS).sort(), Object.keys(python.presets).sort()), 'the same preset names');
for (const name of Object.keys(python.presets)) {
  check(same(INTERVAL_PRESETS[name], python.presets[name]), `preset ${name} has the same ranges`);
}
for (const list of lists) {
  check(same(resolveIntervals(list), python.resolved[list]), `"${list}" resolves the same`);
}

const covers = (intervals, cp) => intervals.some(([s, e]) => s <= cp && cp <= e);
for (const [cp, what] of [[0x20AC, 'euro sign'], [0x2122, 'trade mark sign'], [0x2116, 'numero sign'],
                          [0x2212, 'minus sign']]) {
  for (const list of ['latin-ext', 'latin-cyrillic', 'builtin,latin-cyrillic,greek-letters,musical-symbols']) {
    check(covers(resolveIntervals(list), cp), `${list} covers the ${what}`);
  }
}

console.log(failures ? `${failures} failed` : 'all passed');
process.exit(failures ? 1 : 0);
