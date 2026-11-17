import { expect, test } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";

const uniq = () => Math.random().toString(36).slice(2, 7);

test.describe("assistant pages", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("routines: create, switch off and skip today", async ({ page }) => {
    const errors = watchConsole(page);
    const title = `Evening review ${uniq()}`;
    await page.goto("/assistant");
    await expect(page).toHaveURL(/\/assistant\/routines$/);
    await page.getByRole("button", { name: "New routine" }).click();
    const sheet = page.getByRole("dialog", { name: "New routine" });
    await sheet.getByRole("tab", { name: "Evening" }).click();
    await sheet.getByLabel("Name").fill(title);
    await sheet.getByLabel("Time").fill("21:30");
    await sheet.getByLabel("Earliest (adaptive)").fill("20:30");
    await sheet.getByLabel("Latest (adaptive)").fill("22:30");
    await sheet.getByRole("group", { name: "On" }).getByRole("button", { name: "Sun" }).click();
    await sheet.getByLabel("What it should do").fill("Review the day and **plan tomorrow**.");
    await sheet.getByRole("button", { name: "Add routine" }).click();
    const card = page.getByRole("listitem").filter({ hasText: title });
    await expect(card.getByText("21:30")).toBeVisible();
    await expect(card.locator("strong", { hasText: "plan tomorrow" })).toBeVisible();
    await card.getByRole("button", { name: "Skip today" }).click();
    await expect(card.getByText("Skipped today")).toBeVisible();
    await card.getByRole("switch", { name: `${title} enabled` }).click();
    await expect(card.getByRole("switch", { name: `${title} enabled` })).toHaveAttribute("aria-checked", "false");
    await expectAccessible(page);
    expect(errors).toEqual([]);
  });

  test("reminders: add a repeating one and mark it done", async ({ page }) => {
    const text = `Call mom ${uniq()}`;
    await page.goto("/assistant/reminders");
    await page.getByRole("button", { name: "New reminder" }).first().click();
    const sheet = page.getByRole("dialog", { name: "New reminder" });
    await sheet.getByLabel("Remind me to").fill(text);
    await sheet.getByLabel("Where").selectOption("telegram");
    await sheet.getByLabel("Repeats").check();
    await sheet.getByLabel("Frequency").selectOption("DAILY");
    await sheet.getByRole("button", { name: "Add reminder" }).click();
    const pending = page.getByRole("list", { name: "Pending reminders" });
    const row = pending.getByRole("listitem").filter({ hasText: text });
    await expect(row).toContainText(/every day/i);
    await expect(row).toContainText("Telegram");
    await expectAccessible(page);
    await pending.getByRole("button", { name: `Done: ${text}` }).click();
    await expect(page.getByRole("list", { name: "Earlier reminders" }).getByText(text)).toBeVisible();
  });

  test("memory: search, expand earlier versions and forget", async ({ page }) => {
    // Facts are only ever written by the assistant, so this serves them (and the forget) from a fixture.
    const meta = { created_at: "2026-11-01T00:00:00Z", updated_at: "2026-11-01T00:00:00Z", version: 1, created_by: { kind: "agent" }, origin: "inferred" };
    let facts = [
      { ...meta, id: "f1", statement: "Goes to the gym on Monday, Wednesday and Friday", confidence: 0.9, status: "active", valid_from: "2026-10-01T00:00:00Z", last_used_at: "2026-11-15T08:00:00Z" },
      { ...meta, id: "f0", statement: "Goes to the gym on Monday and Thursday", confidence: 0.8, status: "superseded", superseded_by: "f1", valid_from: "2026-08-01T00:00:00Z" },
      { ...meta, id: "f2", statement: "Priya is a close friend from college", confidence: 0.95, status: "active", valid_from: "2026-09-01T00:00:00Z" },
    ];
    const deleted: string[] = [];
    await page.route(/\/api\/v1\/memory\/facts(\/[^?]*)?(\?.*)?$/, async (route) => {
      const url = new URL(route.request().url());
      if (route.request().method() === "DELETE") {
        const id = url.pathname.split("/").pop()!;
        deleted.push(id);
        facts = facts.filter((f) => f.id !== id);
        return route.fulfill({ status: 204 });
      }
      return route.fulfill({ json: { items: facts, next_cursor: null } });
    });
    await page.goto("/assistant/memory");
    await expect(page.getByText("Goes to the gym on Monday, Wednesday and Friday")).toBeVisible();
    await expect(page.getByText("Goes to the gym on Monday and Thursday")).toHaveCount(0);
    await page.getByRole("button", { name: "Earlier versions of: Goes to the gym on Monday, Wednesday and Friday" }).click();
    await expect(page.getByRole("list", { name: "Earlier versions" }).getByText("Goes to the gym on Monday and Thursday")).toBeVisible();
    await page.getByLabel("Search memory").fill("priya");
    await expect(page.getByRole("list", { name: "Facts" }).getByRole("listitem")).toHaveCount(1);
    await expectAccessible(page);
    await page.getByRole("button", { name: "Forget: Priya is a close friend from college" }).click();
    await page.getByRole("dialog", { name: "Forget this?" }).getByRole("button", { name: "Forget" }).click();
    await expect(page.getByText("Forgotten")).toBeVisible();
    expect(deleted).toEqual(["f2"]);
  });
});
