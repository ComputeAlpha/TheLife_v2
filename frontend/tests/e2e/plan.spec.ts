import { expect, test } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";

const uniq = () => Math.random().toString(36).slice(2, 7);

test.describe("goals, plans and targets", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("build a goal tree, a plan for it and a target", async ({ page }) => {
    const errors = watchConsole(page);
    const tag = uniq();
    const year = `Become a better engineer ${tag}`;
    const quarter = `Ship TheLife v2 ${tag}`;
    await page.goto("/plan");
    await expect(page.getByRole("heading", { name: "Goals & plans" })).toBeVisible();

    // A year goal.
    await page.getByRole("button", { name: "New goal" }).click();
    let sheet = page.getByRole("dialog", { name: "New goal" });
    await sheet.getByLabel("Goal").fill(year);
    await sheet.getByRole("tab", { name: "Year" }).click();
    await sheet.getByRole("radio", { name: "Skill" }).click();
    await sheet.getByLabel("Why it matters").fill("Depth over breadth.");
    await sheet.getByRole("button", { name: "Add goal" }).click();
    const yearRow = page.getByRole("button", { name: new RegExp(`^${year}`) });
    await expect(yearRow).toBeVisible();

    // A quarter goal under it, from the row's own button.
    await page.getByRole("button", { name: `Add a quarter goal under ${year}` }).click();
    sheet = page.getByRole("dialog", { name: "New quarter goal" });
    await expect(sheet.getByText(`Under “${year}”`)).toBeVisible();
    await sheet.getByLabel("Goal").fill(quarter);
    await sheet.getByLabel("Due").fill("2026-12-31");
    await sheet.getByRole("button", { name: "Add goal" }).click();
    // Nested under the year goal, which now counts it.
    await expect(page.getByRole("list", { name: `Inside ${year}` }).getByRole("button", { name: new RegExp(`^${quarter}`) })).toBeVisible();
    await expect(yearRow).toContainText("1 sub-goal");
    await expect(page.getByRole("button", { name: `Sub-goals of ${year}` })).toHaveAttribute("aria-expanded", "true");
    await expectAccessible(page);

    // A plan for the quarter goal, written in Markdown.
    await page.getByRole("tab", { name: /Plans/ }).click();
    await page.getByRole("button", { name: "New plan" }).click();
    sheet = page.getByRole("dialog", { name: "New plan" });
    await sheet.getByLabel("Title").fill(`Release week ${tag}`);
    await sheet.getByLabel("For goal").selectOption({ label: `Quarter: ${quarter}` });
    await sheet.getByLabel("Plan body (Markdown)").fill("## Focus\n- **Two** deep-work blocks a day");
    await sheet.getByRole("tab", { name: "Preview" }).click();
    await expect(sheet.locator("strong", { hasText: "Two" })).toBeVisible();
    await sheet.getByRole("button", { name: "Add plan" }).click();
    const card = page.locator("[id^=plan-]", { hasText: `Release week ${tag}` });
    await expect(card).toBeVisible();
    await expect(card.getByText(quarter)).toBeVisible();
    await expect(card.locator("strong", { hasText: "Two" })).toBeVisible();
    await expectAccessible(page);

    // A daily target tied to the year goal.
    await page.getByRole("tab", { name: /Targets/ }).click();
    await page.getByRole("button", { name: "New target" }).click();
    sheet = page.getByRole("dialog", { name: "New target" });
    await sheet.getByLabel("Target").fill(`Read ${tag}`);
    await sheet.getByRole("radio", { name: "Skill" }).click();
    await sheet.getByLabel("Amount").fill("45");
    await sheet.getByRole("button", { name: "Workday" }).click();
    await sheet.getByLabel("For goal").selectOption({ label: `Year: ${year}` });
    await sheet.getByText("Track as a habit").click();
    await sheet.getByRole("button", { name: "Add target" }).click();
    const target = page.locator("[id^=target-]", { hasText: `Read ${tag}` });
    await expect(target).toContainText("45 minutes a day");
    await expect(target.getByLabel("Streak tracked")).toBeVisible();
    await expectAccessible(page);

    // The goal row now counts what hangs off it; deleting the plan takes it away.
    await page.getByRole("tab", { name: /Goals/ }).click();
    await expect(page.getByRole("button", { name: new RegExp(`^${quarter}`) })).toContainText("1 plan");
    await expect(yearRow).toContainText("1 target");
    await page.getByRole("tab", { name: /Plans/ }).click();
    await card.getByRole("button", { name: `Release week ${tag}` }).click();
    await page.getByRole("dialog", { name: "Edit plan" }).getByRole("button", { name: "Delete" }).click();
    await page.getByRole("button", { name: "Delete for good" }).click();
    await expect(card).toHaveCount(0);
    expect(errors).toEqual([]);
  });

  test("a goal can't be put under a shorter horizon or its own child", async ({ page }) => {
    const tag = uniq();
    const r1 = await page.request.post("/api/v1/goals", { data: { title: `Month ${tag}`, area: "work", horizon: "month" }, headers: { "X-TheLife-CSRF": "1" } });
    const month = await r1.json();
    await page.request.post("/api/v1/goals", { data: { title: `Week ${tag}`, area: "work", horizon: "week", parent_id: month.id }, headers: { "X-TheLife-CSRF": "1" } });
    await page.goto(`/plan?goal=${month.id}`);
    await page.locator(`#goal-${month.id}`).getByRole("button", { name: new RegExp(`^Month ${tag}`) }).click();
    const sheet = page.getByRole("dialog", { name: "Edit goal" });
    const options = await sheet.getByLabel("Part of").locator("option").allTextContents();
    expect(options.some((o) => o.includes(`Week ${tag}`))).toBe(false);
    expect(options.some((o) => o.includes(`Month ${tag}`))).toBe(false);
    expect(options.every((o) => o === "Nothing (top level)" || /^(Year|Quarter):/.test(o))).toBe(true);
  });
});
