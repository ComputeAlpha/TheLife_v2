import { setupServer } from "msw/node";
import { http, HttpResponse } from "msw";
import type { User } from "@/api/types";

export const testUser: User = {
  id: "0190a8f2-0000-7000-8000-000000000001",
  email: "asha@example.com",
  display_name: "Asha Rao",
  timezone: "Asia/Kolkata",
  day_start_hour: 4,
  role: "user",
  workdays: ["MO", "TU", "WE", "TH", "FR"],
};

export function problem(status: number, title: string, extra: Record<string, unknown> = {}) {
  return HttpResponse.json(
    { type: `https://thelife.local/problems/${title.toLowerCase().replaceAll(" ", "-")}`, title, status, request_id: "req-1", ...extra },
    { status, headers: { "Content-Type": "application/problem+json" } },
  );
}

export const server = setupServer(
  http.get("*/api/v1/system/llm-status", () => HttpResponse.json({ mode: "online", providers: [], resets_at: null })),
);
export { http, HttpResponse };
