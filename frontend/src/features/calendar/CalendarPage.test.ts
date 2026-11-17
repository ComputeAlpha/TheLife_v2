import { step, viewRange } from "./CalendarPage";

const WD = ["MO", "TU", "WE", "TH", "FR"];

describe("calendar view ranges", () => {
  it("covers the right days per view", () => {
    expect(viewRange("day", "2026-11-18", WD)).toMatchObject({ days: ["2026-11-18"], from: "2026-11-18", to: "2026-11-19" });
    const w = viewRange("week", "2026-11-18", WD);
    expect(w.days).toHaveLength(7);
    expect(w.from).toBe("2026-11-16");
    expect(viewRange("workweek", "2026-11-18", ["MO", "WE", "SA"]).days).toEqual(["2026-11-16", "2026-11-18", "2026-11-21"]);
    const m = viewRange("month", "2026-11-18", WD);
    expect(m.days).toHaveLength(42);
    expect(m.to).toBe("2026-12-07");
    expect(viewRange("year", "2026-11-18", WD)).toMatchObject({ from: "2026-01-01", to: "2027-01-01" });
  });
  it("steps by the view's unit", () => {
    expect(step("week", "2026-11-18", 1)).toBe("2026-11-25");
    expect(step("month", "2026-01-31", 1)).toBe("2026-02-28");
    expect(step("year", "2026-11-18", -1)).toBe("2025-11-18");
    expect(step("agenda", "2026-11-18", 1)).toBe("2026-12-18");
  });
  it("falls back to the full week when no workdays are set", () => {
    expect(viewRange("workweek", "2026-11-18", []).days).toHaveLength(7);
  });
});
