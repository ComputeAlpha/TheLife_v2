import { describe, expect, it } from "vitest";
import type { Task } from "@/api/types";
import { groupOf, sortTasks } from "./tasks";

const tz = "Asia/Kolkata";
const now = Date.parse("2026-11-17T06:30:00Z"); // 12:00 IST
const t = (p: Partial<Task>): Task => ({ id: p.title ?? "x", title: "x", area: "work", status: "open", version: 1, created_at: "", updated_at: "2026-11-01T00:00:00Z", created_by: { kind: "user" }, origin: "direct", ...p });

describe("task groups", () => {
  it("puts each task in one group", () => {
    expect(groupOf(t({ due_at: "2026-11-17T12:00:00Z" }), now, "2026-11-17", tz)).toBe("today"); // 17:30 IST
    expect(groupOf(t({ due_at: "2026-11-17T19:00:00Z" }), now, "2026-11-17", tz)).toBe("upcoming"); // 00:30 IST tomorrow
    expect(groupOf(t({ due_at: "2026-11-17T05:00:00Z" }), now, "2026-11-17", tz)).toBe("overdue");
    expect(groupOf(t({}), now, "2026-11-17", tz)).toBe("nodate");
    expect(groupOf(t({ rrule: "FREQ=DAILY", due_at: "2026-11-01T00:00:00Z" }), now, "2026-11-17", tz)).toBe("recurring");
    expect(groupOf(t({ status: "done", due_at: "2026-11-01T00:00:00Z" }), now, "2026-11-17", tz)).toBe("done");
  });
  it("today runs to the next day start, not midnight", () => {
    const late = Date.parse("2026-11-16T21:00:00Z"); // 02:30 IST on the 17th, still the 16th with a 04:00 day start
    expect(groupOf(t({ due_at: "2026-11-16T22:00:00Z" }), late, "2026-11-16", tz, 4)).toBe("today"); // 03:30 IST
    expect(groupOf(t({ due_at: "2026-11-16T23:00:00Z" }), late, "2026-11-16", tz, 4)).toBe("upcoming"); // 04:30 IST
  });
  it("sorts by due, then priority", () => {
    const list = [t({ title: "b", due_at: "2026-11-18T00:00:00Z" }), t({ title: "a", due_at: "2026-11-18T00:00:00Z", priority: 3 }), t({ title: "c", due_at: "2026-11-17T00:00:00Z" })];
    expect(sortTasks(list, "upcoming").map((x) => x.title)).toEqual(["c", "a", "b"]);
  });
});
