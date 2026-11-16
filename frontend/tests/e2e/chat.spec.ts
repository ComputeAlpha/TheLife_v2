import { expect, test, type Page } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";
import { resetLlm, script } from "./fake-llm";

const H = { "X-TheLife-CSRF": "1" };
const uniq = () => Math.random().toString(36).slice(2, 7);

async function api(page: Page, method: "get" | "post", path: string, data?: unknown) {
  const r = await page.request[method](`/api/v1${path}`, { data, headers: H });
  expect(r.ok(), `${method} ${path}: ${await r.text()}`).toBeTruthy();
  return r.json();
}

// One model at a time: tests script the shared fake in order.
test.describe.configure({ mode: "serial" });

test.describe("chat", () => {
  test.beforeEach(async ({ page }) => {
    await resetLlm();
    await signIn(page);
  });

  test("a message streams a reply and the tool call shows as a step", async ({ page }) => {
    const errors = watchConsole(page);
    const title = `Read 20 pages ${uniq()}`;
    await script([
      { content: "", tool_calls: [{ name: "agent_load_tools", arguments: { group: "planning" } }] },
      { content: "", tool_calls: [{ name: "tasks_create", arguments: { title, area: "skill", rrule: "FREQ=DAILY" } }] },
      { content: `Added **${title}** as a daily task. You've got this.`, delay_ms: 150 },
    ]);
    await page.goto("/chat");
    await page.getByLabel("Message").fill("Add a daily reading habit of 20 pages");
    await page.keyboard.press("Enter");
    await expect(page.getByText("Add a daily reading habit of 20 pages").first()).toBeVisible();
    await expect(page.getByText(`Added ${title} as a daily task.`, { exact: false })).toBeVisible({ timeout: 15_000 });
    const step = page.getByRole("button", { name: /Create task/ }).last();
    await expect(step).toBeVisible();
    await step.click();
    await expect(page.getByText(`"title": "${title}"`).last()).toBeVisible();
    // The assistant's change is live elsewhere too.
    const tasks = await api(page, "get", "/tasks?recurring=true&limit=200");
    expect(tasks.items.some((t: { title: string }) => t.title === title)).toBe(true);
    await expectAccessible(page);
    expect(errors).toEqual([]);
  });

  test("a gated change waits for approval inline and runs when approved", async ({ page, isMobile }) => {
    const title = `Call the bank ${uniq()}`;
    const task = await api(page, "post", "/tasks", { title, area: "personal" });
    await script([
      { content: "", tool_calls: [{ name: "agent_load_tools", arguments: { group: "planning" } }] },
      { content: "", tool_calls: [{ name: "tasks_delete", arguments: { id: task.id } }] },
      { content: "That needs your okay, it's in the chat." },
      { content: "Deleted it." },
    ]);
    await page.goto("/chat");
    await page.getByLabel("Message").fill(`delete ${title}`);
    await page.getByRole("button", { name: "Send" }).click();
    let id = "";
    await expect.poll(async () => {
      const p = await api(page, "get", "/approvals?status=pending");
      id = p.items.find((x: { target_id?: string }) => x.target_id === task.id)?.id ?? "";
      return id;
    }, { timeout: 15_000 }).not.toBe("");
    // It appears in the thread live, without a reload.
    const card = page.locator(`#approval-${id}`);
    await expect(card).toBeVisible({ timeout: 15_000 });
    await expect(card.getByText("Waiting for you")).toBeVisible();
    await expect(card.getByText(`“${title}”`)).toBeVisible(); // named, not an id
    if (!isMobile) await expect(page.getByRole("link", { name: /Approvals.*waiting/ })).toBeVisible();
    await card.getByRole("button", { name: "Approve" }).click();
    await expect(page.getByText("Approved and done").first()).toBeVisible();
    await expect(card.getByText("Approved")).toBeVisible();
    expect((await page.request.get(`/api/v1/tasks/${task.id}`)).status()).toBe(404);
  });

  test("rejecting from the Approvals page keeps the item and records the reason", async ({ page }) => {
    const title = `Old notes ${uniq()}`;
    const task = await api(page, "post", "/tasks", { title, area: "personal" });
    await script([
      { content: "", tool_calls: [{ name: "agent_load_tools", arguments: { group: "planning" } }] },
      { content: "", tool_calls: [{ name: "tasks_delete", arguments: { id: task.id } }] },
      { content: "Asked for approval." },
      { content: "Okay, keeping it." },
    ]);
    await api(page, "post", "/chat/web/messages", { text: `remove ${title}` });
    await page.goto("/approvals");
    let id = "";
    await expect.poll(async () => {
      const p = await api(page, "get", "/approvals?status=pending");
      id = p.items.find((x: { target_id?: string }) => x.target_id === task.id)?.id ?? "";
      return id;
    }, { timeout: 15_000 }).not.toBe("");
    await page.reload();
    const card = page.locator(`#approval-${id}`);
    await card.getByRole("button", { name: "Reject" }).click();
    await card.getByLabel("Why reject? (optional)").fill("still need it");
    await card.getByRole("button", { name: "Reject now" }).click();
    await expect(page.getByText("Rejected: nothing changed").first()).toBeVisible();
    await page.getByRole("tab", { name: "History" }).click();
    await expect(page.getByText("Reason: still need it").first()).toBeVisible();
    expect((await page.request.get(`/api/v1/tasks/${task.id}`)).status()).toBe(200);
    await expectAccessible(page);
  });

  test("the quick chat answers from any page", async ({ page }) => {
    await script([{ content: "Quick answer from the side sheet." }]);
    await page.goto("/calendar/week");
    await page.getByRole("button", { name: "Ask the assistant" }).click();
    const sheet = page.getByRole("dialog", { name: "Ask the assistant" });
    await sheet.getByLabel("Message").fill("hello there");
    await sheet.getByLabel("Message").press("Enter");
    await expect(sheet.getByText("Quick answer from the side sheet.")).toBeVisible({ timeout: 15_000 });
  });

  test("an approval link from a desktop notification lands on the card", async ({ page }) => {
    const task = await api(page, "post", "/tasks", { title: `Linked ${uniq()}`, area: "personal" });
    await script([
      { content: "", tool_calls: [{ name: "agent_load_tools", arguments: { group: "planning" } }] },
      { content: "", tool_calls: [{ name: "tasks_delete", arguments: { id: task.id } }] },
      { content: "Waiting for your okay." },
    ]);
    // Earlier chat today fills the page, so the card starts out of view.
    await api(page, "post", "/chat/web/messages", { text: "delete the linked task" });
    let id = "";
    await expect.poll(async () => {
      const p = await api(page, "get", "/approvals?status=pending");
      id = p.items.find((x: { target_id?: string }) => x.target_id === task.id)?.id ?? "";
      return id;
    }, { timeout: 15_000 }).not.toBe("");
    await page.goto("/");
    await page.goto(`/chat#approval-${id}`);
    const card = page.locator(`#approval-${id}`).first();
    await expect(card).toBeInViewport({ timeout: 10_000 });
    await expect(card).toHaveClass(/ring-accent/);
  });
});
