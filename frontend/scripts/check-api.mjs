// Fails when src/api/schema.d.ts is out of date with api/generated/openapi.yaml (run `npm run gen:api`).
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import yaml from "js-yaml";

const dir = mkdtempSync(path.join(tmpdir(), "thelife-api-"));
const out = path.join(dir, "schema.d.ts");
try {
  execFileSync("npx", ["openapi-typescript", "../api/generated/openapi.yaml", "--default-non-nullable", "false", "-o", out], { stdio: "ignore" });
  if (readFileSync(out, "utf8") !== readFileSync("src/api/schema.d.ts", "utf8")) {
    console.error("src/api/schema.d.ts is stale: run `npm run gen:api`");
    process.exit(1);
  }
  console.log("API types match the contract");
  // The client's list of POSTs without an Idempotency-Key must be exactly the contract's.
  const spec = yaml.load(readFileSync("../api/generated/openapi.yaml", "utf8"));
  const want = Object.entries(spec.paths)
    .filter(([, item]) => item.post && !(item.post.parameters ?? []).some((x) => String(x.$ref ?? "").endsWith("/IdempotencyKey")))
    .map(([p]) => p).sort();
  const client = readFileSync("src/api/client.ts", "utf8");
  const block = client.slice(client.indexOf("NO_IDEMPOTENCY_KEY = new Set(["));
  const have = [...block.slice(0, block.indexOf("]);")).matchAll(/"([^"]+)"/g)].map((m) => m[1]).sort();
  if (JSON.stringify(want) !== JSON.stringify(have)) {
    console.error(`NO_IDEMPOTENCY_KEY in src/api/client.ts is out of date.\n  contract: ${want.join(", ")}\n  client:   ${have.join(", ")}`);
    process.exit(1);
  }
  console.log("Idempotency-Key exemptions match the contract");
} finally {
  rmSync(dir, { recursive: true, force: true });
}
