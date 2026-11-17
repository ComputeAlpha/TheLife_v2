import type { CalendarEntry } from "@/api/types";
import { bucketByDay, daysOf, windows } from "./entries";

const tz = "Asia/Kolkata";
const e = (over: Partial<CalendarEntry>): CalendarEntry => ({
  kind: "event", id: "1", title: "x", starts_at: "2026-11-16T04:30:00Z", ends_at: "2026-11-16T05:30:00Z",
  all_day: false, area: "work", status: null, occurs_on: null, ...over,
});

describe("calendar entries", () => {
  it("places a timed entry on its local day with minutes", () => {
    const b = bucketByDay([e({})], ["2026-11-16"], tz).get("2026-11-16")!;
    expect(b.timed[0]).toMatchObject({ start: 600, end: 660 });
  });
  it("splits an overnight entry across both days", () => {
    const night = e({ starts_at: "2026-11-16T16:30:00Z", ends_at: "2026-11-17T01:30:00Z" }); // 22:00–07:00
    expect(daysOf(night, tz)).toEqual(["2026-11-16", "2026-11-17"]);
    const m = bucketByDay([night], ["2026-11-16", "2026-11-17"], tz);
    expect(m.get("2026-11-16")!.timed[0]).toMatchObject({ start: 1320, end: 1860 });
    expect(m.get("2026-11-17")!.timed[0]).toMatchObject({ start: -120, end: 420 });
  });
  it("puts a recurring task occurrence on its occurs_on day, all day", () => {
    const t = e({ kind: "task_occurrence", all_day: true, occurs_on: "2026-11-17", starts_at: "2026-11-16T22:30:00Z", ends_at: "2026-11-17T22:30:00Z" });
    expect(daysOf(t, tz)).toEqual(["2026-11-17"]);
  });
  it("an entry ending exactly at midnight stays on one day", () => {
    expect(daysOf(e({ starts_at: "2026-11-16T17:30:00Z", ends_at: "2026-11-16T18:30:00Z" }), tz)).toEqual(["2026-11-16"]);
  });
  it("splits a year into /calendar-sized windows", () => {
    const w = windows("2026-01-01", "2027-01-01");
    expect(w).toHaveLength(6);
    expect(w[0]).toEqual(["2026-01-01", "2026-03-04"]);
    expect(w.at(-1)![1]).toBe("2027-01-01");
  });
});
