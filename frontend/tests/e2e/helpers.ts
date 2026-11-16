import { readFileSync } from "node:fs";
import { expect, type Page } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";
import { credsFile } from "./global-setup";

export interface Creds { tenant: string; email: string; password: string }
export const creds = (): Creds => JSON.parse(readFileSync(credsFile, "utf8"));

export async function signIn(page: Page, next = "/") {
  const c = creds();
  await page.goto(next === "/" ? "/login" : `/login?next=${encodeURIComponent(next)}`);
  await page.getByLabel("Email").fill(c.email);
  await page.getByLabel("Password").fill(c.password);
  await page.getByRole("button", { name: /sign in/i }).click();
  await expect(page.getByRole("main")).toBeVisible();
}

/** Fails on serious or critical accessibility violations. */
export async function expectAccessible(page: Page) {
  const r = await new AxeBuilder({ page }).analyze();
  const bad = r.violations.filter((v) => v.impact === "serious" || v.impact === "critical");
  expect(bad.map((v) => `${v.id}: ${v.help} (${v.nodes.length})`)).toEqual([]);
}

/** Collects console errors and CSP violations so tests can assert there are none. */
export function watchConsole(page: Page): string[] {
  const errors: string[] = [];
  // The signed-out check of /me answers 401 by design; the browser logs every failed request.
  page.on("console", (m) => m.type() === "error" && !/status of 401/.test(m.text()) && errors.push(m.text()));
  page.on("pageerror", (e) => errors.push(String(e)));
  return errors;
}
