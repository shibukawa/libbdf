// Writes what the reference's TypeScript reader makes of the structure of
// every page layer of the reference's test documents (extractContent: the
// nodes, the node and the language of each run, the node of each link), for
// go/structure_test.go to compare libbdf with. The Go implementation has no
// structure, so this is the oracle.
//
//   node tools/structure-oracle.mjs <bdf checkout> go/testdata/structure.txt.gz [testdata]
//
// The checkout must have packages/core built (npm install && npm run build).
// testdata is the directory of the documents (default: the checkout's): give
// that of the module go/go.mod pins, so that the test reads the same ones,
//
//   "$(go list -C go -m -f '{{.Dir}}' github.com/shibukawa/bdf)/testdata"
//
// With --objects, it reads pairs of objects instead (a line of JSON each:
// {"top": base64, "child": base64}; a part whose name starts with the byte 1
// resolves to the child) and writes what it makes of each to the standard
// output: go/structure_test.go's TestStructureRandom runs it so.
//
//   node tools/structure-oracle.mjs <bdf checkout> --objects pairs.jsonl
import { readFile, readdir, writeFile } from "node:fs/promises";
import { join, relative } from "node:path";
import { pathToFileURL } from "node:url";
import { gzipSync } from "node:zlib";

const [root, out, data] = process.argv.slice(2);
if (!root || !out) {
  console.error("usage: structure-oracle.mjs <bdf checkout> <out.txt.gz> [testdata]");
  process.exit(2);
}
const { BufferSource, BdfDocument, extractContent, decodeObject } = await import(pathToFileURL(join(root, "packages/core/dist/index.js")).href);

// a number as the float libbdf gives, to the bit
const f32 = new Float32Array(1), u32 = new Uint32Array(f32.buffer);
const bits = (v) => { if (Number.isNaN(v)) return "nan"; f32[0] = v; return u32[0].toString(16).padStart(8, "0"); };
// a row or a column as libbdf's uint32_t
const index = (v) => (v > 0 ? Math.min(Math.floor(v), 0xffffffff) : 0);

async function files(dir) {
  const found = [];
  for (const e of await readdir(dir, { withFileTypes: true })) {
    if (e.isDirectory()) found.push(...await files(join(dir, e.name)));
    else if (e.name.endsWith(".bdf")) found.push(join(dir, e.name));
  }
  return found.sort();
}

function dump(c) {
  const lines = [];
  c.nodes.forEach((n, i) => {
    let s = `N ${i} ${n.kind} ${n.parent}`;
    if (n.level !== undefined) s += ` level=${n.level}`;
    if (n.row !== undefined) s += ` cell=${index(n.row)},${index(n.col)},${index(n.rows)},${index(n.cols)}`;
    if (n.scope !== undefined) s += ` scope=${n.scope}`;
    if (n.alt !== undefined) s += ` alt=${JSON.stringify(n.alt)}`;
    if (n.bounds) s += ` bounds=${bits(n.bounds.x)},${bits(n.bounds.y)},${bits(n.bounds.w)},${bits(n.bounds.h)}`;
    lines.push(s);
  });
  // runs of one node and one language in a row are written as a range
  const key = (r) => `${r.node} ${r.lang === undefined ? "-" : JSON.stringify(r.lang)}`;
  let from = 0;
  for (let i = 1; i <= c.runs.length; i++) {
    if (i === c.runs.length || key(c.runs[i]) !== key(c.runs[from])) {
      lines.push(`R ${from} ${i - 1} ${key(c.runs[from])}`);
      from = i;
    }
  }
  c.links.forEach((k, i) => lines.push(`L ${i} ${k.node} ${k.after}`));
  return lines;
}

async function objects() {
  const lines = [];
  const pairs = (await readFile(data, "utf8")).split("\n").filter((s) => s);
  for (const [i, line] of pairs.entries()) {
    const pair = JSON.parse(line);
    try {
      const top = decodeObject(new Uint8Array(Buffer.from(pair.top, "base64")));
      const child = pair.child ? decodeObject(new Uint8Array(Buffer.from(pair.child, "base64"))) : undefined;
      const c = extractContent(top, (h) => (h.startsWith("01") ? child : undefined));
      lines.push(`== ${i} ${c.nodes.length} ${c.runs.length} ${c.links.length}`, ...dump(c));
    } catch (e) {
      lines.push(`== ${i} error ${e.constructor.name}`);
    }
  }
  // the whole answer in one write: a process that exits before a pipe took
  // everything cuts it short
  await new Promise((done) => process.stdout.write(lines.join("\n") + "\n", done));
}

async function documents() {
  const testdata = data ?? join(root, "testdata");
  const lines = [];
  for (const file of await files(testdata)) {
    const name = relative(testdata, file).split("\\").join("/");
    if (name.split("/").length > 2 || name.startsWith("segments/")) continue;
    let doc;
    try {
      const bytes = new Uint8Array(await readFile(file));
      doc = await BdfDocument.open(new BufferSource(bytes), name === "demo-encrypted.bdf" ? { password: "demo-パスワード" } : {});
    } catch (e) {
      continue;
    }
    for (const [v, view] of (doc.manifest.views ?? []).entries()) {
      for (const [p, page] of (view.pages ?? []).entries()) {
        for (const [l, layer] of page.layers.entries()) {
          let c;
          try {
            c = extractContent(await doc.ensure(layer.obj), (h) => doc.objectSync(h));
          } catch (e) {
            lines.push(`== ${name} ${v} ${p} ${l} ${layer.obj} error`);
            continue;
          }
          lines.push(`== ${name} ${v} ${p} ${l} ${layer.obj} ${c.nodes.length} ${c.runs.length} ${c.links.length}`);
          lines.push(...dump(c));
        }
      }
    }
  }
  await writeFile(out, gzipSync(lines.join("\n") + "\n", { level: 9 }));
  console.error(`${lines.filter((s) => s.startsWith("== ")).length} objects, ${lines.length} lines`);
}

await (out === "--objects" ? objects() : documents());
