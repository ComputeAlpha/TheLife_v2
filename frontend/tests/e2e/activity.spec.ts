import { expect, test, type Page } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";
import { at, dayAt, freshPastDay } from "./seed";

const H = { "X-TheLife-CSRF": "1" };
const uniq = () => Math.random().toString(36).slice(2, 7);

async function post(page: Page, path: string, data: unknown) {
  const r = await page.request.post(`/api/v1${path}`, { data, headers: H });
  expect(r.ok(), `${path}: ${await r.text()}`).toBeTruthy();
  return r.json();
}

function detection(page: Page, d: number, h1: number, m1: number, h2: number, m2: number, area: string, summary: string, confidence = 0.8) {
  return post(page, "/detections", { source: "external", area, category: area === "work" ? "coding" : "browsing", summary, started_at: at(d, h1, m1), ended_at: at(d, h2, m2), confidence });
}

/** The detail view: a side panel on desktop, a sheet on phones. */
const details = (page: Page, isMobile: boolean) =>
  isMobile ? page.getByRole("dialog", { name: "Detection" }) : page.getByRole("complementary", { name: "Detection details" });

test.describe("activity review", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("confirm, dispute and correct detections on the timeline", async ({ page, isMobile }) => {
    const errors = watchConsole(page);
    const d = freshPastDay();
    const tag = uniq();
    await detection(page, d, 9, 0, 10, 30, "work", `Editing scheduler.cc ${tag}`, 0.9);
    await detection(page, d, 11, 0, 11, 45, "entertainment", `YouTube ${tag}`, 0.7);
    await detection(page, d, 14, 0, 15, 0, "entertainment", `Reddit ${tag}`, 0.6);
    await post(page, "/events", { title: `Deep work ${tag}`, starts_at: at(d, 9, 0), ends_at: at(d, 12, 0), area: "work" });

    await page.goto(`/activity/${dayAt(d)}`);
    await expect(page.getByRole("heading", { name: "Activity" })).toBeVisible();
    // Counted: 90 + 45 + 60 minutes, merged per area.
    await expect(page.getByText("3 h 15 min", { exact: true })).toBeVisible();
    await expect(page.getByRole("tab", { name: /3 to review/ })).toBeVisible();
    // The plan shows behind as a ghost.
    await expect(page.getByRole("list", { name: "Planned" }).getByText(`Deep work ${tag}`)).toBeVisible();
    await expectAccessible(page);

    // Confirm.
    await page.getByRole("button", { name: new RegExp(`Editing scheduler.cc ${tag}`) }).click();
    let panel = details(page, isMobile);
    await panel.getByRole("button", { name: "Confirm" }).click();
    await expect(panel.getByText("Confirmed").first()).toBeVisible();
    if (isMobile) await page.getByRole("dialog", { name: "Detection" }).getByRole("button", { name: "Close", exact: true }).click();

    // Dispute with a reason.
    await page.getByRole("button", { name: new RegExp(`YouTube ${tag}`) }).click();
    panel = details(page, isMobile);
    await panel.getByRole("button", { name: "Dispute" }).click();
    await panel.getByLabel("What was wrong? (optional)").fill("It was a conference talk for work");
    await panel.getByRole("button", { name: "Dispute" }).click();
    await expect(panel.getByText("Under review").first()).toBeVisible();
    await expect(panel.getByText("“It was a conference talk for work”")).toBeVisible();
    if (isMobile) await page.getByRole("dialog", { name: "Detection" }).getByRole("button", { name: "Close", exact: true }).click();

    // Correct: say what really happened.
    await page.getByRole("button", { name: new RegExp(`Reddit ${tag}`) }).click();
    panel = details(page, isMobile);
    await panel.getByRole("button", { name: "Correct" }).click();
    await panel.getByRole("radio", { name: "Skill" }).click();
    await panel.getByLabel("What you were doing").fill(`Reading the Rust book ${tag}`);
    await panel.getByLabel("Category").fill("reading");
    await panel.getByRole("button", { name: "Save correction" }).click();
    await expect(page.getByText("Corrected").first()).toBeVisible();
    // The panel moves on to the correction itself.
    await expect(details(page, isMobile).getByRole("heading", { name: `Reading the Rust book ${tag}` })).toBeVisible();
    if (isMobile) await page.getByRole("dialog", { name: "Detection" }).getByRole("button", { name: "Close", exact: true }).click();
    // The correction replaces the original on the timeline; the original is back with "Show corrections".
    await expect(page.getByRole("button", { name: new RegExp(`^Reading the Rust book ${tag}`) })).toBeVisible();
    await expect(page.getByRole("button", { name: new RegExp(`^Reddit ${tag}`) })).toHaveCount(0);
    if (isMobile) await page.getByRole("button", { name: "Filters" }).click(); // folded on phones
    await page.getByLabel("Show corrections").check();
    await expect(page.getByRole("button", { name: new RegExp(`^Reddit ${tag}`) })).toBeVisible();
    await page.getByRole("button", { name: new RegExp(`^Reading the Rust book ${tag}`) }).click();
    panel = details(page, isMobile);
    await expect(panel.getByText("Correction chain")).toBeVisible();
    await expectAccessible(page);
    expect(errors).toEqual([]);
  });

  test("bulk confirm what's left to review", async ({ page }) => {
    const d = freshPastDay();
    const tag = uniq();
    await detection(page, d, 8, 0, 8, 30, "work", `Email ${tag}`);
    await detection(page, d, 8, 30, 9, 0, "work", `Slack ${tag}`);
    await detection(page, d, 9, 0, 9, 40, "skill", `Course video ${tag}`);
    await page.goto(`/activity/${dayAt(d)}`);
    await page.getByRole("button", { name: "Select", exact: true }).click();
    await page.getByRole("button", { name: "Select all to review" }).click();
    await expect(page.getByText("3 selected")).toBeVisible();
    await page.getByRole("button", { name: "Confirm 3" }).click();
    await expect(page.getByText("Confirmed 3")).toBeVisible();
    await expect(page.getByRole("tab", { name: /to review/ })).toHaveCount(0);
    const r = await page.request.get(`/api/v1/detections?from=${encodeURIComponent(at(d, 0))}&to=${encodeURIComponent(at(d + 1, 0))}`);
    const items = (await r.json()).items as { status: string; summary: string }[];
    expect(items.filter((x) => x.summary.endsWith(tag)).map((x) => x.status)).toEqual(["confirmed", "confirmed", "confirmed"]);
  });

  test("log, edit and delete an activity by hand", async ({ page }) => {
    const d = freshPastDay();
    const title = `Evening run ${uniq()}`;
    await page.goto(`/activity/${dayAt(d)}`);
    await page.getByRole("tab", { name: "Activities" }).click();
    await expect(page.getByText("Nothing logged this day")).toBeVisible();
    await page.getByRole("button", { name: "Log activity" }).first().click();
    const sheet = page.getByRole("dialog", { name: "Log activity" });
    await sheet.getByLabel("What did you do?").fill(title);
    await sheet.getByRole("radio", { name: "Fitness" }).click();
    await sheet.getByLabel("Category").fill("running");
    await sheet.getByLabel("From").fill(`${dayAt(d)}T18:00`);
    await sheet.getByLabel("To").fill(`${dayAt(d)}T18:40`);
    await sheet.getByRole("button", { name: "Log it" }).click();
    await expect(page.getByRole("button", { name: new RegExp(title) })).toContainText("40 min");
    await expectAccessible(page);

    await page.getByRole("button", { name: new RegExp(title) }).click();
    const edit = page.getByRole("dialog", { name: "Edit activity" });
    await edit.getByLabel("To").fill(`${dayAt(d)}T19:00`);
    await edit.getByRole("button", { name: "Save" }).click();
    await expect(page.getByRole("button", { name: new RegExp(title) })).toContainText("1 h");

    await page.getByRole("button", { name: new RegExp(title) }).click();
    await page.getByRole("dialog", { name: "Edit activity" }).getByRole("button", { name: "Delete" }).click();
    await page.getByRole("button", { name: "Delete for good" }).click();
    await expect(page.getByText("Nothing logged this day")).toBeVisible();
  });

  test("add what you were doing where the collector saw nothing", async ({ page, isMobile }) => {
    const d = freshPastDay();
    const what = `Design review in room 4 ${uniq()}`;
    await detection(page, d, 9, 0, 9, 30, "work", `Email ${uniq()}`);
    await page.goto(`/activity/${dayAt(d)}`);
    await page.getByRole("button", { name: "Add", exact: true }).click();
    const sheet = page.getByRole("dialog", { name: "Add what you were doing" });
    await sheet.getByLabel("What you were doing").fill(what);
    await sheet.getByRole("radio", { name: "Work" }).click();
    await sheet.getByLabel("Category").fill("meeting");
    await sheet.getByLabel("From").fill(`${dayAt(d)}T10:00`);
    await sheet.getByLabel("To").fill(`${dayAt(d)}T11:00`);
    await sheet.getByRole("button", { name: "Add" }).click();
    // It's selected straight away: the detail panel (a sheet on phones) shows it.
    await expect(details(page, isMobile).getByRole("heading", { name: what })).toBeVisible();
    if (isMobile) await page.getByRole("dialog", { name: "Detection" }).getByRole("button", { name: "Close", exact: true }).click();
    await expect(page.getByRole("button", { name: new RegExp(`^${what}`) })).toBeVisible();
  });
});
