import { expect, test } from "@playwright/test";
import { expectAccessible, signIn, watchConsole } from "./helpers";

const uniq = () => Math.random().toString(36).slice(2, 7);

test("roles, responsibilities, projects, deliverables and project tasks", async ({ page }) => {
  const errors = watchConsole(page);
  await signIn(page);
  const tag = uniq();
  const role = `Backend engineer ${tag}`;
  const project = `Payments v2 ${tag}`;
  await page.goto("/work");
  await page.getByRole("button", { name: "New role" }).click();
  let sheet = page.getByRole("dialog", { name: "New role" });
  await sheet.getByLabel("Role", { exact: true }).fill(role);
  await sheet.getByLabel("Organisation").fill("Acme");
  await sheet.getByRole("button", { name: "Add role" }).click();
  const card = page.getByLabel(role, { exact: true });
  await expect(card).toBeVisible();

  await card.getByRole("button", { name: `Add a responsibility to ${role}` }).click();
  sheet = page.getByRole("dialog", { name: "New responsibility" });
  await sheet.getByLabel("Responsibility").fill("Code review");
  await sheet.getByLabel("Repeats").check();
  await sheet.getByLabel("Frequency").selectOption("DAILY");
  await sheet.getByRole("button", { name: "Add" }).click();
  await expect(card.getByRole("button", { name: /Code review/ })).toContainText(/every day/i);

  await card.getByRole("button", { name: `Add a project to ${role}` }).click();
  sheet = page.getByRole("dialog", { name: "New project" });
  await sheet.getByLabel("Project").fill(project);
  await sheet.getByLabel("Due").fill("2027-01-31");
  await sheet.getByRole("button", { name: "Add project" }).click();
  await card.getByRole("link", { name: new RegExp(project) }).click();
  await expect(page.getByRole("heading", { name: project })).toBeVisible();
  await expectAccessible(page);

  await page.getByRole("button", { name: "Add" }).first().click();
  sheet = page.getByRole("dialog", { name: "New deliverable" });
  await sheet.getByLabel("Deliverable").fill("Design doc signed off");
  await sheet.getByLabel("Due").fill("2026-12-20T17:00");
  await sheet.getByRole("button", { name: "Add" }).click();
  await expect(page.getByRole("list", { name: "Deliverables" }).getByText("Design doc signed off")).toBeVisible();

  await page.getByRole("button", { name: "Add" }).nth(1).click();
  sheet = page.getByRole("dialog", { name: "New task" });
  await sheet.getByLabel("Task").fill(`Write the migration ${tag}`);
  await sheet.getByRole("radio", { name: "Work" }).click();
  await expect(sheet.getByLabel("Project")).toHaveValue(/.+/);
  await sheet.getByRole("button", { name: "Add task" }).click();
  await expect(page.getByRole("button", { name: `Write the migration ${tag}` })).toBeVisible();
  expect(errors).toEqual([]);
});
