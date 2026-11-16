import { addDays, addMonths, dayOf, dayStartMs, isValidDay, minutesInto, monthGrid, startOfWeek, weekdayCode } from "./days";

describe("calendar days", () => {
  it("does plain date arithmetic", () => {
    expect(addDays("2026-12-31", 1)).toBe("2027-01-01");
    expect(addMonths("2026-01-31", 1)).toBe("2026-02-28");
    expect(startOfWeek("2026-11-22")).toBe("2026-11-16"); // Sunday → Monday
    expect(weekdayCode("2026-11-16")).toBe("MO");
    expect(isValidDay("2026-02-30")).toBe(false);
  });
  it("builds a Monday-first 6-week month grid", () => {
    const g = monthGrid("2026-11-16");
    expect(g).toHaveLength(42);
    expect(g[0]).toBe("2026-10-26");
    expect(g).toContain("2026-11-30");
  });
  it("converts days to instants in the user's zone, across DST", () => {
    expect(new Date(dayStartMs("2026-11-16", "Asia/Kolkata")).toISOString()).toBe("2026-11-15T18:30:00.000Z");
    // New York leaves DST on 2026-11-01: that day has 25 hours.
    const ny = "America/New_York";
    expect((dayStartMs("2026-11-02", ny) - dayStartMs("2026-11-01", ny)) / 3_600_000).toBe(25);
    expect(dayOf(Date.parse("2026-11-15T19:00:00Z"), "Asia/Kolkata")).toBe("2026-11-16");
    expect(minutesInto(Date.parse("2026-11-16T04:30:00Z"), "2026-11-16", "Asia/Kolkata")).toBe(600);
  });
});
