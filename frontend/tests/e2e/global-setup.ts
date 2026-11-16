// Creates a throwaway tenant and user in the test database through thelife-admin (as accounts are made for real).
import { execFileSync } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
export const credsFile = path.join(root, "frontend/test-results/.e2e-user.json");

export default function globalSetup() {
  const adminUrl = process.env.THELIFE_TEST_ADMIN_DB_URL;
  if (!adminUrl) throw new Error('THELIFE_TEST_ADMIN_DB_URL not set: run eval "$(scripts/db.sh test)"');
  const admin = process.env.E2E_ADMIN_BIN ?? path.join(root, "build/dev/backend/thelife-admin");
  const env = { ...process.env, THELIFE_ADMIN_DB_URL: adminUrl };
  const tenant = execFileSync(admin, ["tenant", "create", "--name", "e2e-web"], { env, encoding: "utf8" }).match(/[0-9a-f-]{36}/)?.[0];
  if (!tenant) throw new Error("tenant create printed no id");
  const email = `web-${Date.now()}@e2e.test`;
  const out = execFileSync(admin, ["user", "create", "--tenant", tenant, "--email", email, "--name", "Asha Rao", "--timezone", "Asia/Kolkata"], { env, encoding: "utf8" });
  const password = out.match(/password:\s*(\S+)/i)?.[1];
  if (!password) throw new Error("user create printed no password");
  mkdirSync(path.dirname(credsFile), { recursive: true });
  writeFileSync(credsFile, JSON.stringify({ tenant, email, password }), { mode: 0o600 });
}
