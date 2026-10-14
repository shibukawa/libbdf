// Writes what the reference's TypeScript reader finds in the text index
// parts of the reference's test documents (TextSearch of
// packages/core/src/search.ts), for go/search_test.go to compare libbdf
// with. The Go implementation has no search, so this is the oracle.
//
//   node tools/search-oracle.mjs <bdf checkout> go/testdata/search.txt.gz [testdata]
//
// The checkout must have packages/core built. testdata is the directory of
// the documents (default: the checkout's): give that of the module go/go.mod
// pins, "$(go list -C go -m -f '{{.Dir}}' github.com/shibukawa/bdf)/testdata".
//
// With --indexes, it reads text index parts and queries instead (a line of
// JSON each: {"index": base64, "queries": [...], "caseSensitive": bool}) and
// writes the hits to the standard output: TestSearchRandom runs it so. With
// --normalize, it reads texts (a line of JSON each) and writes what a search
// makes of each, without and with case: TestSearchNormalizeAll runs it so.
//
// The offsets of a hit are written in bytes of the run's UTF-8, as libbdf
// gives them. The context of a hit is left out where the searchable text has
// characters outside the BMP: the reference counts it in UTF-16 units, libbdf
// in characters.
import { createHash } from "node:crypto";
import { readFile, readdir, writeFile } from "node:fs/promises";
import { join, relative } from "node:path";
import { pathToFileURL } from "node:url";
import { gzipSync } from "node:zlib";

const [root, out, data] = process.argv.slice(2);
if (!root || !out) {
  console.error("usage: search-oracle.mjs <bdf checkout> <out.txt.gz> [testdata]");
  process.exit(2);
}
const { BufferSource, BdfDocument, TextSearch, decodeTextIndex, normalizeQuery } = await import(pathToFileURL(join(root, "packages/core/dist/index.js")).href);

const LIMIT = 4, CONTEXT = 12;
const bytes = (s) => Buffer.byteLength(s, "utf8");
const astral = (s) => /[\u{10000}-\u{10ffff}]/u.test(s);

// The hits of the queries in a decoded index, as lines.
function search(runs, queries, caseSensitive) {
  const ts = new TextSearch(runs, caseSensitive);
  const plain = ts.text;
  const lines = [`T ${createHash("sha256").update(plain, "utf8").digest("hex").slice(0, 16)} ${astral(plain) ? 1 : 0}`];
  for (const q of queries) {
    const hits = ts.search(q, { limit: LIMIT, context: CONTEXT });
    lines.push(`Q ${JSON.stringify(q)} ${JSON.stringify(normalizeQuery(q, caseSensitive))} ${hits.length}`);
    for (const h of hits) {
      lines.push(`H ${JSON.stringify(h.text)} ${astral(plain) ? "-" : JSON.stringify(h.context)}`);
      for (const s of h.segments) {
        const text = runs[s.run].text;
        lines.push(`S ${s.run} ${s.a} ${s.b} ${s.ordinal} ${bytes(text.slice(0, s.start))} ${bytes(text.slice(0, s.end))}`);
      }
    }
  }
  return lines;
}

async function indexes() {
  const lines = [];
  const cases = (await readFile(data, "utf8")).split("\n").filter((s) => s);
  for (const [i, line] of cases.entries()) {
    const c = JSON.parse(line);
    try {
      const runs = decodeTextIndex(new Uint8Array(Buffer.from(c.index, "base64")));
      lines.push(`== ${i} ${c.caseSensitive ? 1 : 0} ${runs.length}`, ...search(runs, c.queries, c.caseSensitive));
    } catch (e) {
      lines.push(`== ${i} error ${e.constructor.name}`);
    }
  }
  await new Promise((done) => process.stdout.write(lines.join("\n") + "\n", done));
}

async function normalized() {
  const line = (s) => JSON.stringify(s);
  const lines = [];
  for (const text of (await readFile(data, "utf8")).split("\n")) {
    if (text === "") continue;
    const s = JSON.parse(text);
    lines.push(`${line(normalizeQuery(s, false))} ${line(normalizeQuery(s, true))}`);
  }
  await new Promise((done) => process.stdout.write(lines.join("\n") + "\n", done));
}

// A small generator of numbers, the same on every machine.
function random(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), a | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Queries for a text: pieces of it as it is and as a reader might type them,
// and a few that every text is asked.
function queriesFor(runs, rnd) {
  const all = runs.map((r) => r.text).filter((t) => t.length > 0);
  const queries = ["a", " ", "の", "-", "A  B", "１", "\u2212", ""];
  for (let i = 0; i < 6 && all.length; i++) {
    const text = [...all[Math.floor(rnd() * all.length)]];
    const from = Math.floor(rnd() * text.length), n = 1 + Math.floor(rnd() * 8);
    const piece = text.slice(from, from + n).join("");
    queries.push(i % 3 === 0 ? piece.toUpperCase() : i % 3 === 1 ? piece.normalize("NFKD") : piece);
  }
  // across two runs
  for (let i = 0; i < 2 && runs.length > 1; i++) {
    const k = 1 + Math.floor(rnd() * (runs.length - 1));
    const a = [...runs[k - 1].text].slice(-3).join(""), b = [...runs[k].text].slice(0, 3).join("");
    queries.push(runs[k].sep === 1 ? `${a} ${b}` : a + b);
  }
  return queries;
}

async function files(dir) {
  const found = [];
  for (const e of await readdir(dir, { withFileTypes: true })) {
    if (e.isDirectory()) found.push(...await files(join(dir, e.name)));
    else if (e.name.endsWith(".bdf")) found.push(join(dir, e.name));
  }
  return found.sort();
}

async function documents() {
  const testdata = data ?? join(root, "testdata");
  const lines = [];
  let seed = 1;
  for (const file of await files(testdata)) {
    const name = relative(testdata, file).split("\\").join("/");
    if (name.split("/").length > 2 || name.startsWith("segments/")) continue;
    let doc;
    try {
      const raw = new Uint8Array(await readFile(file));
      doc = await BdfDocument.open(new BufferSource(raw), name === "demo-encrypted.bdf" ? { password: "demo-パスワード" } : {});
    } catch (e) {
      continue;
    }
    for (const [v, view] of (doc.manifest.views ?? []).entries()) {
      if (!view.textIndex) continue;
      const runs = decodeTextIndex(await doc.part(view.textIndex));
      const caseSensitive = seed % 4 === 0;
      lines.push(`== ${name} ${v} ${view.textIndex} ${caseSensitive ? 1 : 0} ${runs.length}`);
      lines.push(...search(runs, queriesFor(runs, random(seed++)), caseSensitive));
    }
  }
  await writeFile(out, gzipSync(lines.join("\n") + "\n", { level: 9 }));
  console.error(`${lines.filter((s) => s.startsWith("== ")).length} indexes, ${lines.filter((s) => s.startsWith("H ")).length} hits, ${lines.length} lines`);
}

await (out === "--indexes" ? indexes() : out === "--normalize" ? normalized() : documents());
