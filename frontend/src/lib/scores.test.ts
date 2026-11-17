import { describe, expect, it } from "vitest";
import { adjustmentPoints, periodEnd, periodLabel, periodStart, periodStarts } from "./scores";

describe("score periods", () => {
  it("finds the containing period start", () => {
    expect(periodStart("day", "2026-11-18")).toBe("2026-11-18");
    expect(periodStart("week", "2026-11-18")).toBe("2026-11-16"); // Monday
    expect(periodStart("month", "2026-11-18")).toBe("2026-11-01");
  });
  it("lists the window oldest first", () => {
    expect(periodStarts("day", "2026-11-18", 3)).toEqual(["2026-11-16", "2026-11-17", "2026-11-18"]);
    expect(periodStarts("week", "2026-11-18", 2)).toEqual(["2026-11-09", "2026-11-16"]);
    expect(periodStarts("month", "2026-01-10", 3)).toEqual(["2025-11-01", "2025-12-01", "2026-01-01"]);
    expect(periodStarts("day", "2026-11-18")).toHaveLength(30);
  });
  it("knows where a period ends", () => {
    expect(periodEnd("week", "2026-11-16")).toBe("2026-11-22");
    expect(periodEnd("month", "2026-02-01")).toBe("2026-02-28");
    expect(periodLabel("week", "2026-11-16", true)).toBe("Week of 16 Nov – 22 Nov 2026");
  });
  it("converts the adjustment percentage to points", () => {
    expect(adjustmentPoints(10)).toBe(3);
    expect(adjustmentPoints(-30)).toBe(-9);
    expect(adjustmentPoints(null)).toBe(0);
  });
});
