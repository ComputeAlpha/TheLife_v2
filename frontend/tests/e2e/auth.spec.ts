import { expect, test } from "@playwright/test";
import { creds, expectAccessible, signIn, watchConsole } from "./helpers";

test("signed-out visitors land on the login page and come back after signing in", async ({ page }) => {
  const errors = watchConsole(page);
  await page.goto("/settings/profile");
  await expect(page).toHaveURL(/\/login\?next=%2Fsettings%2Fprofile$/);
  await expectAccessible(page);
  const c = creds();
  await page.getByLabel("Email").fill(c.email);
  await page.getByLabel("Password").fill(c.password);
  await page.getByRole("button", { name: /sign in/i }).click();
  await expect(page).toHaveURL(/\/settings\/profile$/);
  await expect(page.getByRole("heading", { name: "Settings" })).toBeVisible();
  // The session is an HttpOnly cookie: scripts can't see it.
  expect(await page.evaluate(() => document.cookie)).not.toContain("tl_session");
  const cookie = (await page.context().cookies()).find((k) => k.name === "tl_session");
  expect(cookie?.httpOnly).toBe(true);
  expect(cookie?.sameSite).toBe("Strict");
  expect(errors).toEqual([]);
});

test("a wrong password is refused", async ({ page }) => {
  await page.goto("/login");
  await page.getByLabel("Email").fill(creds().email);
  await page.getByLabel("Password").fill("definitely wrong");
  await page.getByRole("button", { name: /sign in/i }).click();
  await expect(page.getByRole("alert")).toContainText("don't match");
});

test("signing out ends the session", async ({ page }) => {
  await signIn(page);
  await page.getByRole("button", { name: /^Account:/ }).click();
  await page.getByRole("menuitem", { name: "Sign out" }).click();
  await expect(page).toHaveURL(/\/login/);
  await page.goto("/settings");
  await expect(page).toHaveURL(/\/login/);
});

test("the shell navigates and the palette jumps", async ({ page, isMobile }) => {
  const errors = watchConsole(page);
  await signIn(page);
  await expect(page.getByRole("heading", { level: 1 })).toContainText("Asha");
  await expectAccessible(page);
  if (!isMobile) {
    await page.keyboard.press("Control+k");
    await page.getByPlaceholder("Jump to, or ask the assistant…").fill("settings");
    await page.keyboard.press("Enter");
    await expect(page).toHaveURL(/\/settings\/profile$/);
    await page.keyboard.press("g");
    await page.keyboard.press("t");
    await expect(page).toHaveURL(/\/$/);
  }
  expect(errors).toEqual([]);
});

test("profile changes are saved", async ({ page }) => {
  await signIn(page, "/settings/profile");
  await page.getByLabel("Display name").fill("Asha R");
  await page.getByRole("button", { name: "Sat" }).click();
  await page.getByRole("button", { name: "Save changes" }).click();
  await expect(page.getByText("Profile saved", { exact: true })).toBeVisible();
  await page.reload();
  await expect(page.getByLabel("Display name")).toHaveValue("Asha R");
  await expect(page.getByRole("button", { name: "Sat" })).toHaveAttribute("aria-pressed", "true");
  // Put it back for the other tests.
  await page.getByLabel("Display name").fill("Asha Rao");
  await page.getByRole("button", { name: "Sat" }).click();
  await page.getByRole("button", { name: "Save changes" }).click();
  await expect(page.getByText("Profile saved", { exact: true })).toBeVisible();
});
