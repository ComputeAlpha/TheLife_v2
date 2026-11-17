// Seeds a believable week for the signed-in test user through the public API (the page's own cookie session).
import type { APIRequestContext } from "@playwright/test";

const H = { "X-TheLife-CSRF": "1" };

async function post(req: APIRequestContext, path: string, data: unknown) {
  const r = await req.post(`/api/v1${path}`, { data, headers: H });
  if (!r.ok()) throw new Error(`${path}: ${r.status()} ${await r.text()}`);
  return r.json();
}

/** Local wall time in Asia/Kolkata (+05:30) on a day offset from today, as an ISO instant. */
export function at(dayOffset: number, hh: number, mm = 0): string {
  const now = new Date(Date.now() + 5.5 * 3_600_000); // shift to IST wall clock
  const d = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() + dayOffset, hh, mm));
  return new Date(d.getTime() - 5.5 * 3_600_000).toISOString();
}

export async function seedWeek(req: APIRequestContext, tag = "") {
  const ev = (title: string, d: number, h1: number, m1: number, h2: number, m2: number, area: string, extra: object = {}) =>
    post(req, "/events", { title: title + tag, starts_at: at(d, h1, m1), ends_at: at(d, h2, m2), area, ...extra });
  await ev("Deep work: backend", 0, 9, 0, 11, 30, "work");
  await ev("Standup", 0, 10, 0, 10, 30, "work");
  await ev("Design review", 0, 10, 15, 11, 0, "work");
  await ev("Gym", 0, 18, 0, 19, 0, "fitness", { rrule: "FREQ=WEEKLY;BYDAY=MO,WE,FR" });
  await ev("Lunch with Priya", 1, 13, 0, 14, 0, "personal", { location: "Koramangala" });
  await ev("C++ course", 1, 20, 0, 21, 30, "skill");
  await ev("Team offsite", 2, 0, 0, 0, 0, "work", { all_day: true, starts_at: at(2, 0), ends_at: at(3, 0) });
  await ev("Movie night", 3, 21, 0, 23, 30, "entertainment");
  await post(req, "/tasks", { title: "Read 20 pages" + tag, area: "skill", rrule: "FREQ=DAILY", rrule_dtstart: at(-3, 8), track_consistency: true });
  await post(req, "/tasks", { title: "Stretch 10 min" + tag, area: "fitness", rrule: "FREQ=DAILY", rrule_dtstart: at(-3, 8) });
  await post(req, "/tasks", { title: "Pay electricity bill" + tag, area: "personal", due_at: at(0, 17, 0) });
  await post(req, "/tasks", { title: "Send weekly report" + tag, area: "work", due_at: at(1, 16, 0), priority: 2 });
  const item = await post(req, "/entertainment/items", { kind: "game", title: "Hades II" + tag, status: "playing" });
  await post(req, "/entertainment/slots", { item_id: item.id, starts_at: at(4, 20, 0), ends_at: at(4, 22, 0) });
  const book = await post(req, "/skills/resources", { kind: "book", title: "Designing Data-Intensive Applications" + tag, total_pages: 600 });
  await post(req, "/skills/sessions", { resource_id: book.id, started_at: at(-1, 7, 0), ended_at: at(-1, 8, 0), page_from: 1, page_to: 30 });
  await post(req, "/detections", { source: "user", area: "work", category: "coding", summary: "Editing calendar.cc in VS Code" + tag, started_at: at(0, 9, 5), ended_at: at(0, 10, 0), confidence: 0.9 });
  await post(req, "/reminders", { text: "Call mom" + tag, due_at: at(0, 20, 0) });
}

/** The local day (Asia/Kolkata) at an offset from today, as YYYY-MM-DD. */
export function dayAt(dayOffset: number): string {
  return at(dayOffset, 12).slice(0, 10);
}

/** A past day no other test uses (the desktop and phone projects share one user). */
export function freshPastDay(): number {
  return -(30 + Math.floor(Math.random() * 3000));
}
