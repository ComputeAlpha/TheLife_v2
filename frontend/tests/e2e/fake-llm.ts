import { llmPort } from "../../playwright.config";

const base = `http://127.0.0.1:${llmPort}`;

/** Queues the model's next replies (docker/fake-llm/fake_llm.py). */
export async function script(responses: unknown[]) {
  const r = await fetch(`${base}/_fake/script`, { method: "POST", body: JSON.stringify({ responses }), headers: { "Content-Type": "application/json" } });
  if (!r.ok) throw new Error(`fake-llm script: ${r.status}`);
}
export async function resetLlm() {
  await fetch(`${base}/_fake/reset`, { method: "POST" });
}
