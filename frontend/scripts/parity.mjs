// The demo-UI retirement gate (frontend_lld.md §7): every operation a signed-in user can call over REST must be
// called somewhere in the web UI through the typed client. Usage: node scripts/parity.mjs [--list]
import { readFileSync, readdirSync, statSync } from "node:fs";
import path from "node:path";
import yaml from "js-yaml";

// Operations the web UI deliberately doesn't call, with the reason.
const ALLOW = {
  "auth.login": "bearer-token login for scripts; the web UI uses auth.web_session_create",
  "auth.logout": "bearer-token logout; the web UI uses auth.web_session_delete",
};
const ALLOW_PREFIX = { "ingest.": "collector-only (device principal)", "devices.self.": "collector-only (device principal)" };

const spec = yaml.load(readFileSync("../api/generated/openapi.yaml", "utf8"));
const required = [];
for (const [p, item] of Object.entries(spec.paths)) {
  for (const [method, op] of Object.entries(item)) {
    if (!op || typeof op !== "object" || !op.operationId) continue;
    const tool = op["x-thelife-tool"];
    // Operations without the extension (auth, system) are REST-only and callable by users.
    const principals = tool?.principals ?? ["user"];
    const exposure = tool?.exposure ?? ["rest"];
    if (!principals.includes("user") || !exposure.includes("rest")) continue;
    required.push({ id: op.operationId, method: method.toUpperCase(), path: p });
  }
}

function walk(dir, out = []) {
  for (const f of readdirSync(dir)) {
    const full = path.join(dir, f);
    if (statSync(full).isDirectory()) walk(full, out);
    else if (/\.(ts|tsx)$/.test(f) && !/\.test\.tsx?$/.test(f) && !f.endsWith(".d.ts")) out.push(full);
  }
  return out;
}
const called = new Set();
const sse = new Set();
for (const f of walk("src")) {
  const src = readFileSync(f, "utf8");
  for (const m of src.matchAll(/api\.(GET|POST|PUT|PATCH|DELETE)\(\s*"([^"]+)"/g)) called.add(`${m[1]} ${m[2]}`);
  // The event stream is opened with EventSource, not the typed client.
  for (const m of src.matchAll(/new EventSource\(\s*["`]([^"`?$]+)/g)) sse.add(m[1]);
}

const isCalled = (op) => called.has(`${op.method} ${op.path}`) || (op.method === "GET" && sse.has(op.path));
const byId = new Map(required.map((op) => [op.id, op]));
const missing = [];
const allowed = [];
let viaList = 0;
for (const op of required) {
  const reason = ALLOW[op.id] ?? Object.entries(ALLOW_PREFIX).find(([p]) => op.id.startsWith(p))?.[1];
  if (reason) { allowed.push(`${op.id}: ${reason}`); continue; }
  if (isCalled(op)) continue;
  // A `.get` adds nothing over its collection's `.list`, which returns the same full rows the UI opens items
  // from (the demo's item page showed exactly that row).
  const list = op.id.endsWith(".get") ? byId.get(op.id.replace(/\.get$/, ".list")) : undefined;
  if (list && isCalled(list)) { viaList++; continue; }
  missing.push(op);
}

// A hook that calls an operation only counts if some screen uses it.
const sources = walk("src").map((f) => [f, readFileSync(f, "utf8")]);
const unusedHooks = [];
for (const [f, src] of sources.filter(([f]) => f.startsWith(path.join("src", "api")))) {
  for (const m of src.matchAll(/export function (use\w+)/g)) {
    const re = new RegExp(`\\b${m[1]}\\b`);
    if (!sources.some(([g, other]) => g !== f && re.test(other)) && (src.match(new RegExp(`\\b${m[1]}\\b`, "g")) ?? []).length < 2) {
      unusedHooks.push(`${m[1]} (${f})`);
    }
  }
}

if (process.argv.includes("--list")) for (const a of allowed) console.log(`allowed  ${a}`);
console.log(`${required.length - allowed.length - missing.length}/${required.length - allowed.length} user operations covered (${viaList} gets through their list; ${allowed.length} allow-listed)`);
for (const m of missing) console.error(`missing  ${m.id.padEnd(36)} ${m.method} ${m.path}`);
for (const h of unusedHooks) console.error(`unused   ${h}: no screen calls it`);
if (missing.length || unusedHooks.length) process.exit(1);
