import { expect, test, type Page } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";
import { at, dayAt } from "./seed";

const H = { "X-TheLife-CSRF": "1" };
const uniq = () => Math.random().toString(36).slice(2, 7);
async function api(page: Page, method: "get" | "post", path: string, data?: unknown) {
  const r = await page.request[method](`/api/v1${path}`, { data, headers: H });
  expect(r.ok(), `${method} ${path}: ${await r.text()}`).toBeTruthy();
  return r.json();
}

test.describe("tasks", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("quick add, complete and find a task in Done", async ({ page }) => {
    const errors = watchConsole(page);
    const title = `Buy batteries ${uniq()}`;
    await page.goto("/tasks");
    await page.getByLabel("Quick add a task").fill(title);
    await page.keyboard.press("Enter");
    const list = page.getByRole("list", { name: "Today tasks" });
    await expect(list.getByRole("button", { name: new RegExp(`^${title}`) })).toBeVisible();
    await expectAccessible(page);
    await list.getByRole("button", { name: `Mark ${title} done` }).click();
    await expect(page.getByText(`Done: ${title}`).first()).toBeVisible();
    await page.getByRole("tab", { name: /Done/ }).click();
    await expect(page.getByRole("button", { name: `Reopen ${title}` })).toBeVisible();
    expect(errors).toEqual([]);
  });

  test("the recurrence builder previews dates and saves a weekly rule", async ({ page }) => {
    const title = `Water the plants ${uniq()}`;
    await page.goto("/tasks?g=recurring");
    await page.getByRole("button", { name: "New task" }).click();
    const sheet = page.getByRole("dialog", { name: "New task" });
    await sheet.getByLabel("Task").fill(title);
    await sheet.getByRole("radio", { name: "Personal" }).click();
    await sheet.getByLabel("Repeats").check();
    await sheet.getByLabel("Interval").fill("2");
    const days = sheet.getByRole("group", { name: "On these days" });
    for (const d of ["Mon", "Thu"]) {
      const chip = days.getByRole("button", { name: d });
      if ((await chip.getAttribute("aria-pressed")) !== "true") await chip.click();
    }
    // Drop any other preselected day (the start day's).
    for (const d of ["Tue", "Wed", "Fri", "Sat", "Sun"]) {
      const chip = days.getByRole("button", { name: d });
      if ((await chip.getAttribute("aria-pressed")) === "true") await chip.click();
    }
    await expect(sheet.getByRole("list", { name: "Next dates" }).getByRole("listitem")).toHaveCount(5);
    await expect(sheet.getByText(/every 2 weeks on Monday, Thursday/i)).toBeVisible();
    await expectAccessible(page);
    await sheet.getByRole("button", { name: "Add task" }).click();
    await expect(page.getByRole("button", { name: new RegExp(title) })).toContainText(/every 2 weeks/i);
    const tasks = await api(page, "get", "/tasks?recurring=true&limit=200");
    expect(tasks.items.find((t: { title: string }) => t.title === title).rrule).toBe("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,TH");
  });

  test("a repeating task's past occurrence can be completed from the strip", async ({ page }) => {
    const title = `Stretch ${uniq()}`;
    const task = await api(page, "post", "/tasks", { title, area: "fitness", rrule: "FREQ=DAILY", rrule_dtstart: at(-5, 7) });
    await page.goto(`/tasks?g=recurring&task=${task.id}`);
    const sheet = page.getByRole("dialog", { name: "Edit task" });
    const strip = sheet.getByRole("region", { name: "Occurrences" });
    const day = dayAt(-2);
    const cell = strip.getByRole("button", { name: new RegExp(`${Number(day.slice(8))} [A-Z][a-z]+: (todo|missed)`) });
    await cell.click();
    await strip.getByRole("button", { name: "Complete" }).click();
    await expect(strip.getByRole("button", { name: new RegExp(`${Number(day.slice(8))} [A-Z][a-z]+: done`) })).toBeVisible();
    const occ = await api(page, "get", `/tasks/${task.id}/occurrences?from=${day}&to=${day}`);
    expect(occ.items[0].status).toBe("done");
  });

  test("delete a task and find another from the command palette", async ({ page, isMobile }) => {
    const keep = `Renew passport ${uniq()}`;
    const drop = `Old idea ${uniq()}`;
    await api(page, "post", "/tasks", { title: keep, area: "personal" });
    const gone = await api(page, "post", "/tasks", { title: drop, area: "personal" });
    await page.goto(`/tasks?g=nodate&task=${gone.id}`);
    const sheet = page.getByRole("dialog", { name: "Edit task" });
    await sheet.getByRole("button", { name: "Delete" }).click();
    await sheet.getByRole("button", { name: "Delete for good" }).click();
    await expect(page.getByRole("button", { name: new RegExp(drop) })).toHaveCount(0);
    test.skip(isMobile, "the palette is a keyboard feature");
    await page.keyboard.press("Control+k");
    await page.getByRole("combobox").fill(keep.split(" ").slice(0, 2).join(" ").toLowerCase());
    await expect(page.getByRole("option", { name: new RegExp(keep) })).toBeVisible();
    await page.keyboard.press("Enter");
    await expect(page).toHaveURL(/\/tasks\?task=/);
    await expect(page.getByRole("dialog", { name: "Edit task" }).getByLabel("Task")).toHaveValue(keep);
  });
});
