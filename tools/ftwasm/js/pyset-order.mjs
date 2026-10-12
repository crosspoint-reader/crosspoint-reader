// pyset-order.mjs -- iteration order of a CPython set of small non-negative ints.
//
// fontconvert_sdcard.py collects GSUB lookup indices into a Python set() and
// iterates it; when two lookups map the same input sequence to different
// ligatures, the later one in that order wins. CPython's set order is
// "slot order in an open-addressed hash table", which is NOT ascending once
// indices exceed the table size. This reproduces Objects/setobject.c
// (set_add_entry / set_table_resize / set_insert_clean; CPython 3.7-3.14):
// hash(n) == n for 0 <= n < 2**61-1, PySet_MINSIZE 8, LINEAR_PROBES 9,
// PERTURB_SHIFT 5, resize when fill*5 >= mask*3 to the smallest power of two
// > used*4 (used*2 above 50000). No deletions happen, so there are no dummies.

const MINSIZE = 8;
const LINEAR_PROBES = 9;
const PERTURB_SHIFT = 5n;

function insertClean(table, mask, key) {
  let perturb = BigInt(key);
  let i = key & mask;
  for (;;) {
    if (table[i] === null) { table[i] = key; return; }
    if (i + LINEAR_PROBES <= mask) {
      for (let j = 1; j <= LINEAR_PROBES; j++) {
        if (table[i + j] === null) { table[i + j] = key; return; }
      }
    }
    perturb >>= PERTURB_SHIFT;
    i = Number((BigInt(i) * 5n + 1n + perturb) & BigInt(mask));
  }
}

export class PyIntSet {
  constructor() {
    this.mask = MINSIZE - 1;
    this.table = new Array(MINSIZE).fill(null);
    this.used = 0;
  }

  add(key) {
    if (!Number.isInteger(key) || key < 0 || key > 0x7fffffff) throw new RangeError(`PyIntSet: unsupported key ${key}`);
    const mask = this.mask;
    let i = key & mask;
    let perturb = BigInt(key);
    for (;;) {
      const probes = i + LINEAR_PROBES <= mask ? LINEAR_PROBES : 0;
      for (let p = 0; p <= probes; p++) {
        const k = this.table[i + p];
        if (k === null) {
          this.table[i + p] = key;
          this.used++;
          // fill == used (no deletions)
          if (this.used * 5 >= mask * 3) this.resize(this.used > 50000 ? this.used * 2 : this.used * 4);
          return;
        }
        if (k === key) return;
      }
      perturb >>= PERTURB_SHIFT;
      i = Number((BigInt(i) * 5n + 1n + perturb) & BigInt(mask));
    }
  }

  update(iterable) { for (const k of iterable) this.add(k); }

  resize(minused) {
    let newsize = MINSIZE;
    while (newsize <= minused) newsize *= 2;
    const old = this.table;
    this.table = new Array(newsize).fill(null);
    this.mask = newsize - 1;
    for (const k of old) if (k !== null) insertClean(this.table, this.mask, k);
  }

  has(key) { return this.table.includes(key); }

  get size() { return this.used; }

  *[Symbol.iterator]() { for (const k of this.table) if (k !== null) yield k; }
}
