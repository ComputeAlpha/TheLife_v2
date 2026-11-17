// Scores are computed by background jobs, which the e2e backend runs with off. These tests serve score rows shaped
// exactly as the scoring engine writes them (backend/src/scoring/engine.cc, jobs.cc) for the /scores calls only;
// everything else is the real backend.
import { expect, test } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";
import { serveScores } from "./score-fixtures";
import { dayAt } from "./seed";

test.describe("scores", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("the day view shows the trend, the reasons and the breakdown", async ({ page }) => {
    const errors = watchConsole(page);
    const disputes = await serveScores(page);
    await page.goto("/scores/day");
    // Latest day with a number is selected: 74 + 0.3·13 = 78.
    await expect(page.getByRole("img", { name: "Score 78 of 100" })).toBeVisible();
    await expect(page.getByText("Adjusted +13%")).toBeVisible();
    await expect(page.getByText("The afternoon went to the release, the top goal this month")).toBeVisible();
    await expect(page.getByRole("link", { name: "goal", exact: true })).toHaveAttribute("href", "/plan?goal=g1");
    await expect(page.getByText("Deep work: backend")).toBeVisible();
    await expect(page.getByText("252/240 minutes")).toBeVisible();
    await expect(page.getByText("Nothing planned and no targets: not part of the score.")).toBeVisible();
    await expect(page.getByText("Read 20 pages")).toBeVisible();
    await expect(page.getByText("What got displaced")).toBeVisible();
    await expectAccessible(page);

    // Pick another day from the table view.
    await page.getByRole("button", { name: "Show as table" }).click();
    await page.getByRole("button", { name: new RegExp(`^${new Date(`${dayAt(-3)}T00:00:00Z`).toLocaleDateString("en-GB", { weekday: "long", timeZone: "UTC" })}`) }).click();
    await expect(page).toHaveURL(new RegExp(`/scores/day/${dayAt(-3)}$`));
    await expect(page.getByRole("img", { name: "Score 55 of 100" })).toBeVisible();
    await expect(page.getByText("Adjusted -10%")).toBeVisible();

    // Back to the latest; dispute it and look at the versions.
    await page.goto(`/scores/day/${dayAt(-1)}`);
    await page.getByRole("button", { name: "Dispute" }).click();
    const dlg = page.getByRole("dialog", { name: "Dispute this score" });
    await dlg.getByLabel("What does it miss?").fill("The gym was closed");
    await dlg.getByRole("button", { name: "Send dispute" }).click();
    await expect(page.getByText("Dispute sent")).toBeVisible();
    expect(disputes).toEqual([{ reason: "The gym was closed" }]);
    await page.getByRole("button", { name: "Versions" }).click();
    await expect(page.getByRole("dialog", { name: "Score versions" }).getByText("v1")).toBeVisible();
    await page.keyboard.press("Escape");

    // A day that isn't scored says why instead of showing a zero.
    await page.goto(`/scores/day/${dayAt(-2)}`);
    await expect(page.getByText("The day's profile (holiday) is not scored.")).toBeVisible();
    expect(errors).toEqual([]);
  });

  test("the week view shows coverage, days and weekly targets", async ({ page }) => {
    await serveScores(page);
    await page.goto("/scores/week");
    await expect(page.getByRole("img", { name: "Score 68 of 100" })).toBeVisible();
    await expect(page.getByText("4 of 7 days scored")).toBeVisible();
    await expect(page.getByText("Daily mean 67")).toBeVisible();
    await expect(page.getByText("2/3 sessions")).toBeVisible();
    await expectAccessible(page);
    await page.getByRole("button", { name: /: 78$/ }).click();
    await expect(page).toHaveURL(new RegExp(`/scores/day/${dayAt(-1)}$`));
  });

  test("with no scores it explains when they come", async ({ page }) => {
    await page.route(/\/api\/v1\/scores(\?.*)?$/, (r) => r.fulfill({ json: { items: [], next_cursor: null } }));
    await page.goto("/scores/month");
    await expect(page.getByText("No scores yet")).toBeVisible();
    await expect(page.getByText("A month is scored when it closes, from its day scores.")).toBeVisible();
  });
});
