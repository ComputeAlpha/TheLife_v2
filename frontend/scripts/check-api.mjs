// Fails when src/api/schema.d.ts is out of date with api/generated/openapi.yaml (run `npm run gen:api`).
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

const dir = mkdtempSync(path.join(tmpdir(), "thelife-api-"));
const out = path.join(dir, "schema.d.ts");
try {
  execFileSync("npx", ["openapi-typescript", "../api/generated/openapi.yaml", "--default-non-nullable", "false", "-o", out], { stdio: "ignore" });
  if (readFileSync(out, "utf8") !== readFileSync("src/api/schema.d.ts", "utf8")) {
    console.error("src/api/schema.d.ts is stale: run `npm run gen:api`");
    process.exit(1);
  }
  console.log("API types match the contract");
} finally {
  rmSync(dir, { recursive: true, force: true });
}
