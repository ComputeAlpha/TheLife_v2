import { expect, test, type Page } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";
import { at } from "./seed";

const H = { "X-TheLife-CSRF": "1" };
const uniq = () => Math.random().toString(36).slice(2, 7);

async function api(page: Page, method: "get" | "post", path: string, data?: unknown) {
  const r = await page.request[method](`/api/v1${path}`, { data, headers: H });
  expect(r.ok(), `${method} ${path}: ${await r.text()}`).toBeTruthy();
  return r.json();
}

test.describe("calendar", () => {
  test.beforeEach(async ({ page }) => signIn(page));

  test("drag on the week grid creates an event at that time", async ({ page, isMobile }) => {
    test.skip(isMobile, "pointer drag is a desktop gesture; phones use + New");
    const errors = watchConsole(page);
    const title = `Focus block ${uniq()}`;
    await page.goto("/calendar/week");
    await expect(page.getByRole("tab", { name: "Week", selected: true })).toBeVisible();
    await page.locator("[data-day]").first().waitFor();
    const col = page.locator(`[data-day]`).first();
    await col.scrollIntoViewIfNeeded();
    // 08:00 → 09:30 on Monday of this week: scroll so 08:00 is visible, then drag.
    await page.evaluate(() => document.querySelector("[data-day]")!.closest(".overflow-y-auto")!.scrollTo(0, 7 * 52));
    const box = (await col.boundingBox())!;
    const y = (min: number) => box.y + (min * 52) / 60;
    await page.mouse.move(box.x + box.width / 2, y(8 * 60 + 2));
    await page.mouse.down();
    await page.mouse.move(box.x + box.width / 2, y(9 * 60 + 30), { steps: 6 });
    await page.mouse.up();
    const dialog = page.getByRole("dialog", { name: "New" });
    await expect(dialog).toBeVisible();
    await expect(dialog.getByLabel("Starts")).toHaveValue(/T08:00$/);
    await expect(dialog.getByLabel("Ends")).toHaveValue(/T09:30$/);
    await dialog.getByLabel("Title").fill(title);
    await dialog.getByRole("radio", { name: "Work" }).click();
    await dialog.getByRole("button", { name: "Add" }).click();
    await expect(page.getByRole("button", { name: new RegExp(`^${title}\\s*08:00–09:30`) })).toBeVisible();
    await expectAccessible(page);
    expect(errors).toEqual([]);
  });

  test("day headers line up with their hour columns at any width", async ({ page, isMobile }) => {
    for (const width of [1440, 1100, 800]) {
      await page.setViewportSize({ width, height: 900 });
      await page.goto("/calendar/week");
      // Desktop scrollbars take layout space (the cause of the old drift); playwright.config.ts keeps them on.
      await page.locator("[data-day]").first().waitFor();
      const gutter = await page.evaluate(() => { const s = document.querySelector("[data-day]")!.closest(".overflow-y-auto") as HTMLElement; return s.offsetWidth - s.clientWidth; });
      if (!isMobile) expect(gutter, "desktop scrollbars take space").toBeGreaterThan(0); // phones overlay them
      const heads = await page.locator("[data-head]").evaluateAll((els) => els.map((e) => e.getBoundingClientRect()).map((r) => [r.left, r.width]));
      const cols = await page.locator("[data-day]").evaluateAll((els) => els.map((e) => e.getBoundingClientRect()).map((r) => [r.left, r.width]));
      expect(heads).toHaveLength(cols.length);
      heads.forEach(([l, w], i) => {
        expect(Math.abs(l! - cols[i]![0]!), `left of column ${i} at ${width}px`).toBeLessThan(0.5);
        expect(Math.abs(w! - cols[i]![1]!), `width of column ${i} at ${width}px`).toBeLessThan(0.5);
      });
    }
  });

  test("the create dialog opens in place, without jumping", async ({ page, isMobile }) => {
    test.skip(isMobile, "desktop gesture");
    await page.goto("/calendar/day");
    await page.locator("[data-day]").first().click({ position: { x: 40, y: 600 } });
    const dialog = page.getByRole("dialog", { name: "New" });
    await dialog.waitFor();
    // Sample the dialog's position through its entrance animation: the centre must not move.
    const centres = await dialog.evaluate(async (el) => {
      const out: number[][] = [];
      for (let i = 0; i < 12; i++) {
        const r = el.getBoundingClientRect();
        out.push([r.left + r.width / 2, r.top + r.height / 2]);
        await new Promise((res) => requestAnimationFrame(res));
      }
      return out;
    });
    const [x0, y0] = centres.at(-1)!;
    for (const [x, y] of centres) {
      expect(Math.abs(x! - x0!)).toBeLessThan(1);
      expect(Math.abs(y! - y0!)).toBeLessThan(1);
    }
    await page.keyboard.press("Escape");
  });

  test("dragging an event reschedules it on the server", async ({ page, isMobile }) => {
    test.skip(isMobile, "pointer drag is a desktop gesture");
    const title = `Move me ${uniq()}`;
    const ev = await api(page, "post", "/events", { title, starts_at: at(0, 14, 0), ends_at: at(0, 15, 0), area: "personal" });
    await page.goto("/calendar/day");
    await page.locator("[data-day]").first().waitFor();
    await page.evaluate(() => document.querySelector("[data-day]")!.closest(".overflow-y-auto")!.scrollTo(0, 12 * 52));
    const block = page.getByRole("button", { name: new RegExp(`^${title}\\s*14:00–15:00`) });
    const b = (await block.boundingBox())!;
    await page.mouse.move(b.x + b.width / 2, b.y + 10);
    await page.mouse.down();
    await page.mouse.move(b.x + b.width / 2, b.y + 10 + 52 * 1.5, { steps: 8 }); // +90 min
    await page.mouse.up();
    await expect(page.getByText("Rescheduled", { exact: true })).toBeVisible();
    await expect(page.getByRole("button", { name: new RegExp(`^${title}\\s*15:30–16:30`) })).toBeVisible();
    const after = await api(page, "get", `/events/${ev.id}`);
    expect(Date.parse(after.starts_at)).toBe(Date.parse(at(0, 15, 30)));
    expect(after.version).toBe(2);
  });

  test("a recurring task is completed for one day from its sheet", async ({ page }) => {
    const title = `Meditate ${uniq()}`;
    const task = await api(page, "post", "/tasks", { title, area: "personal", rrule: "FREQ=DAILY", rrule_dtstart: at(-2, 6) });
    await page.goto("/calendar/day");
    await page.getByRole("button", { name: title }).first().click();
    const sheet = page.getByRole("dialog");
    await expect(sheet.getByText(/Repeats every day/)).toBeVisible();
    await sheet.getByRole("button", { name: "Mark done" }).click();
    await expect(page.getByText("Nice, done.", { exact: true })).toBeVisible();
    const today = at(0, 12).slice(0, 10);
    const occ = await api(page, "get", `/tasks/${task.id}/occurrences?from=${today}&to=${today}`);
    expect(occ.items[0].status).toBe("done");
  });

  test("month, year and agenda views show planned entries", async ({ page }) => {
    const errors = watchConsole(page);
    const title = `Dentist ${uniq()}`;
    await api(page, "post", "/events", { title, starts_at: at(1, 11, 0), ends_at: at(1, 11, 45), area: "personal" });
    await page.goto("/calendar/month");
    // The day may be full from earlier tests (then the entry is behind "+N more"); the agenda lists everything.
    await expect(page.getByRole("button", { name: title }).or(page.getByRole("button", { name: /more$/ })).first()).toBeVisible();
    await page.goto("/calendar/agenda");
    await expect(page.getByText(title)).toBeVisible();
    await page.goto("/calendar/year");
    await expect(page.getByRole("region", { name: /January/ })).toBeVisible();
    await expectAccessible(page);
    expect(errors).toEqual([]);
  });

  test("a day can be marked as leave", async ({ page, isMobile }) => {
    test.skip(isMobile, "week headers");
    await page.goto(`/calendar/week/${at(14, 12).slice(0, 10)}`);
    const day = at(14, 12).slice(0, 10);
    await page.getByRole("button", { name: /^Day options for / }).first().click();
    await page.getByRole("menuitem", { name: "leave" }).click();
    await expect(page.getByText(/is now a leave day/).first()).toBeVisible();
    const monday = (await api(page, "get", `/day-profile-overrides?from=${day}&to=${at(21, 12).slice(0, 10)}`)).items;
    expect(monday.some((o: { profile: string }) => o.profile === "leave")).toBe(true);
  });
});

test("today shows the plan and checks tasks off", async ({ page }) => {
  const errors = watchConsole(page);
  await signIn(page);
  const title = `Water plants ${uniq()}`;
  await api(page, "post", "/tasks", { title, area: "personal", due_at: at(0, 23, 50) });
  await page.goto("/");
  await expect(page.getByRole("heading", { name: "Today's plan" })).toBeVisible();
  await page.getByRole("button", { name: `Mark ${title} done` }).click();
  await expect(page.getByText(`Done: ${title}`).first()).toBeVisible();
  await expect(page.getByRole("button", { name: `${title} is done` })).toBeVisible();
  await expectAccessible(page);
  expect(errors).toEqual([]);
});
