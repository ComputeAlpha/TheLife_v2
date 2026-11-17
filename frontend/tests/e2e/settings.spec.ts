import { expect, test } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";
import { dayAt } from "./seed";

const uniq = () => Math.random().toString(36).slice(2, 7);

test.describe("settings", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("register a device and an API key: tokens are shown once", async ({ page, baseURL }) => {
    const errors = watchConsole(page);
    const dev = `Laptop ${uniq()}`;
    await page.goto("/settings/connections");
    await expect(page.getByRole("heading", { name: "Telegram" })).toBeVisible();
    await page.getByRole("button", { name: "Register" }).first().click();
    await page.getByLabel("Device name").fill(dev);
    await page.getByRole("button", { name: "Register" }).last().click();
    let once = page.getByRole("dialog", { name: "Device token" });
    await expect(once.getByLabel("Token", { exact: true })).toHaveText(/^tld_/);
    await expect(once.getByText(`thelife-collector enroll --url ${new URL(baseURL!).origin}`)).toBeVisible();
    await expectAccessible(page);
    await once.getByRole("button", { name: "I've copied it" }).click();
    await expect(page.getByText(dev)).toBeVisible();
    await expect(page.getByText(/tld_/)).toHaveCount(0); // gone for good
    await page.getByRole("listitem").filter({ hasText: dev }).getByRole("button", { name: "Revoke" }).click();
    await page.getByRole("dialog", { name: `Revoke ${dev}?` }).getByRole("button", { name: "Revoke" }).click();
    await expect(page.getByRole("listitem").filter({ hasText: dev }).getByText("Revoked", { exact: true })).toBeVisible();

    const key = `Script ${uniq()}`;
    await page.getByRole("button", { name: "New key" }).click();
    await page.getByLabel("Name").fill(key);
    await page.getByRole("button", { name: "tasks:read" }).click();
    await page.getByRole("button", { name: "calendar:read" }).click();
    await page.getByRole("button", { name: "Create key" }).click();
    once = page.getByRole("dialog", { name: "API key" });
    await expect(once.getByLabel("Token", { exact: true })).toHaveText(/^tlk_/);
    const token = (await once.getByLabel("Token", { exact: true }).textContent())!;
    await once.getByRole("button", { name: "I've copied it" }).click();
    await expect(page.getByRole("listitem").filter({ hasText: key })).toContainText("calendar:read, tasks:read");
    // The key works as a bearer token, within its scopes.
    const r = await page.request.get("/api/v1/tasks?limit=1", { headers: { Authorization: `Bearer ${token}` } });
    expect(r.status()).toBe(200);
    expect(errors).toEqual([]);
  });

  test("tracking: an app rule and work tracking", async ({ page }) => {
    const app = `Zed ${uniq()}`;
    await page.goto("/settings/tracking");
    await page.getByRole("button", { name: "New rule" }).click();
    let sheet = page.getByRole("dialog", { name: "New app rule" });
    await sheet.getByLabel("Window class").fill("dev.zed.Zed");
    await sheet.getByLabel("App").fill(app);
    await sheet.getByLabel("Category").fill("coding");
    await sheet.getByRole("radio", { name: "Work" }).click();
    await sheet.getByRole("button", { name: "Add rule" }).click();
    const rule = page.getByRole("list", { name: "App rules" }).getByRole("listitem").filter({ hasText: app });
    await expect(rule).toContainText("class “dev.zed.Zed”");
    await rule.getByRole("switch").click();
    await expect(rule.getByRole("switch")).toHaveAttribute("aria-checked", "false");

    await page.getByRole("button", { name: "New", exact: true }).click();
    sheet = page.getByRole("dialog", { name: "Set up work tracking" });
    await sheet.getByLabel("Name").fill(`Day job ${uniq()}`);
    await sheet.getByRole("tab", { name: "Self-reported" }).click();
    await sheet.getByLabel("Workday target (hours)").fill("7.5");
    await sheet.getByRole("button", { name: "Add" }).click();
    await expect(page.getByText(/Self-reported · 7.5 h a workday/).first()).toBeVisible();
    await expectAccessible(page);
  });

  test("days: a new kind of day, used for a planned exception", async ({ page }) => {
    const name = `travel${uniq()}`;
    await page.goto("/settings/days");
    await page.getByRole("button", { name: "New kind" }).click();
    const sheet = page.getByRole("dialog", { name: "New kind of day" });
    await sheet.getByLabel("Name").fill(name);
    await sheet.getByLabel("Scored").uncheck();
    await sheet.getByRole("button", { name: "Add" }).click();
    await expect(page.getByRole("button", { name: new RegExp(`${name}.*Not scored`) })).toBeVisible();

    const day = dayAt(40 + Math.floor(Math.random() * 40));
    await page.getByLabel("Day", { exact: true }).fill(day);
    await page.getByLabel("Kind", { exact: true }).selectOption(name);
    await page.getByRole("button", { name: "Set", exact: true }).click();
    const mon = new Date(`${day}T12:00:00Z`).toLocaleString("en-GB", { month: "short", timeZone: "UTC" });
    const sel = page.getByRole("list", { name: "Planned exceptions" }).getByRole("combobox", { name: `Kind of ${Number(day.slice(8))} ${mon}` });
    await expect(sel).toHaveValue(name);
    await sel.selectOption("leave");
    await expect(page.getByText("Changed")).toBeVisible();
    const o = await (await page.request.get(`/api/v1/day-profile-overrides?from=${day}&to=${day}`)).json();
    expect(o.items[0].profile).toBe("leave");
  });

  test("scoring: weights show their shares and save", async ({ page }) => {
    await page.goto("/settings/days");
    // Both projects share one user: always move it to a value different from the current one.
    const slider = page.getByRole("slider", { name: "Consistency" });
    const next = (await slider.inputValue()) === "0" ? "0.25" : "0";
    await slider.fill(next);
    if (next === "0") await expect(page.getByRole("img", { name: /Consistency 0%/ })).toBeVisible();
    await page.getByRole("button", { name: "Save" }).click();
    await expect(page.getByText("Scoring saved")).toBeVisible();
    const s = await (await page.request.get("/api/v1/settings/scoring")).json();
    expect(s.weights.consistency).toBe(Number(next));
    await expectAccessible(page);
  });

  test("notifications, assistant and about render their status", async ({ page }) => {
    await page.goto("/settings/notifications");
    await expect(page.getByText("While the tab is visible")).toBeVisible();
    await page.goto("/settings/assistant");
    await expect(page.getByRole("list", { name: "Providers" })).toBeVisible();
    await expectAccessible(page);
    await page.goto("/settings/about");
    await expect(page.getByText(/^v\d/).first()).toBeVisible();
    await expect(page.getByText("Ready", { exact: true })).toBeVisible();
  });
});
