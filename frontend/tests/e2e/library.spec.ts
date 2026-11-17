import { expect, test } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";

const uniq = () => Math.random().toString(36).slice(2, 7);

test.describe("skills and entertainment", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("a book's progress follows logged sessions", async ({ page }) => {
    const errors = watchConsole(page);
    const title = `Crafting Interpreters ${uniq()}`;
    await page.goto("/skills");
    await page.getByRole("button", { name: "Add book or course" }).first().click();
    const sheet = page.getByRole("dialog", { name: "New book or course" });
    await sheet.getByLabel("Title").fill(title);
    await sheet.getByLabel("Author").fill("Robert Nystrom");
    await sheet.getByLabel("Pages").fill("200");
    await sheet.getByRole("button", { name: "Add" }).click();
    await page.getByRole("link", { name: new RegExp(title) }).click();
    await expect(page.getByRole("heading", { name: title })).toBeVisible();
    await page.getByRole("button", { name: "Log session" }).click();
    const log = page.getByRole("dialog", { name: "Log session" });
    await expect(log.getByLabel("From page")).toHaveValue("1"); // the next page to read

    await log.getByLabel("To page").fill("50");
    await log.getByRole("button", { name: "Log it" }).click();
    await expect(page.getByText("Pages 1–50")).toBeVisible();
    await expect(page.getByRole("img", { name: "25% done" })).toBeVisible();
    await expectAccessible(page);
    expect(errors).toEqual([]);
  });

  test("a course's sections can be added, reordered and marked by a session", async ({ page }) => {
    const title = `Rust course ${uniq()}`;
    await page.goto("/skills");
    await page.getByRole("button", { name: "Add book or course" }).first().click();
    const sheet = page.getByRole("dialog", { name: "New book or course" });
    await sheet.getByRole("tab", { name: "Course" }).click();
    await sheet.getByLabel("Title").fill(title);
    await sheet.getByRole("button", { name: "Add" }).click();
    await page.getByRole("link", { name: new RegExp(title) }).click();
    for (const [s, m] of [["Ownership", "40"], ["Lifetimes", "55"]] as const) {
      await page.getByLabel("New section").fill(s);
      await page.getByLabel("Minutes").fill(m);
      await page.getByRole("button", { name: "Add", exact: true }).click();
      await expect(page.getByRole("list", { name: "Sections" }).getByText(s)).toBeVisible();
    }
    await page.getByRole("button", { name: "Move Lifetimes up" }).click();
    await expect(page.getByRole("list", { name: "Sections" }).getByRole("listitem").first()).toContainText("Lifetimes");
    await page.getByRole("button", { name: "Log session" }).click();
    const log = page.getByRole("dialog", { name: "Log session" });
    await log.getByLabel("Section").selectOption({ label: "2. Ownership" });
    await log.getByRole("button", { name: "Log it" }).click();
    await expect(page.getByRole("list", { name: "Sections" }).getByRole("listitem").nth(1).getByLabel("Started")).toBeVisible();
    await expect(page.getByText("1 of 2 started")).toBeVisible();
  });

  test("plan entertainment, mark it enjoyed and change a status", async ({ page }) => {
    const title = `Outer Wilds ${uniq()}`;
    await page.goto("/entertainment");
    await expect(page.getByText("planned, never limited")).toBeVisible();
    await page.getByRole("button", { name: "Add title" }).first().click();
    const sheet = page.getByRole("dialog", { name: "Add a game, movie or series" });
    await sheet.getByLabel("Title").fill(title);
    await sheet.getByLabel("Status").selectOption("playing");
    await sheet.getByRole("button", { name: "Add" }).click();
    await page.getByRole("button", { name: title }).click();
    await page.getByRole("dialog", { name: "Edit title" }).getByRole("button", { name: "Plan time for it" }).click();
    const slot = page.getByRole("dialog", { name: "Plan entertainment time" });
    await expect(slot.getByLabel("What")).toHaveValue(/.+/);
    await slot.getByRole("button", { name: "Plan it" }).click();
    const row = page.getByRole("listitem").filter({ hasText: title }).filter({ has: page.getByRole("button", { name: /enjoyed/i }) });
    await row.getByRole("button", { name: "Mark enjoyed" }).click();
    await expect(row.getByRole("button", { name: "Enjoyed" })).toHaveAttribute("aria-pressed", "true");
    await page.getByLabel(`Status of ${title}`).selectOption("finished");
    await expect(page.getByLabel(`Status of ${title}`)).toHaveValue("finished");
    await expectAccessible(page);
  });
});
