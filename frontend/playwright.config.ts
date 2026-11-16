import { defineConfig, devices } from "@playwright/test";

const port = Number(process.env.E2E_PORT ?? 18380);
/** The fake OpenAI-compatible model the agent talks to (docker/fake-llm), scripted per test. */
export const llmPort = Number(process.env.E2E_LLM_PORT ?? 18390);

export default defineConfig({
  testDir: "tests/e2e",
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: [["list"]],
  globalSetup: "./tests/e2e/global-setup.ts",
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    trace: "retain-on-failure",
    colorScheme: "dark",
    // Real scrollbars, as on a desktop: they take layout space (headless hides them by default).
    launchOptions: { ignoreDefaultArgs: ["--hide-scrollbars"] },
  },
  projects: [
    { name: "desktop", use: { ...devices["Desktop Chrome"], viewport: { width: 1440, height: 900 } } },
    { name: "phone", use: { ...devices["Pixel 7"] } },
  ],
  webServer: [
    {
      command: `python3 ../docker/fake-llm/fake_llm.py --port ${llmPort}`,
      url: `http://127.0.0.1:${llmPort}/_fake/health`,
      reuseExistingServer: false,
      timeout: 15_000,
    },
    {
      command: "scripts/e2e-backend.sh",
      url: `http://127.0.0.1:${port}/readyz`,
      reuseExistingServer: false,
      timeout: 30_000,
      env: { E2E_PORT: String(port), E2E_LLM_BASE_URL: `http://127.0.0.1:${llmPort}/v1` },
    },
  ],
});
