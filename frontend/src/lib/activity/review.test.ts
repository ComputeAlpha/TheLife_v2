import { describe, expect, it } from "vitest";
import type { Detection } from "@/api/types";
import { applyFilters, areaMinutes, confirmable, isCounted, NO_FILTERS, visibleHours } from "./review";

const base = Date.parse("2026-11-16T00:00:00Z");
const at = (min: number) => new Date(base + min * 60_000).toISOString();
function det(p: Partial<Detection> & { s: number; e: number }): Detection {
  const { s, e, ...rest } = p;
  return {
    id: `d${s}-${e}-${rest.area ?? "work"}-${rest.source ?? "vision"}`,
    created_at: at(0), updated_at: at(0), version: 1, created_by: { kind: "device" }, origin: "direct",
    source: "vision" as Detection["source"], started_at: at(s), ended_at: at(e), area: "work", category: "coding", summary: "x",
    status: "detected", confidence: 0.9, ...rest,
  };
}

describe("isCounted", () => {
  it("counts confirmed and confident detections only", () => {
    expect(isCounted(det({ s: 0, e: 10, status: "confirmed", confidence: 0.1 }))).toBe(true);
    expect(isCounted(det({ s: 0, e: 10, confidence: 0.4 }))).toBe(true);
    expect(isCounted(det({ s: 0, e: 10, confidence: 0.39 }))).toBe(false);
    expect(isCounted(det({ s: 0, e: 10, status: "disputed" }))).toBe(false);
    expect(isCounted(det({ s: 0, e: 10, status: "superseded" }))).toBe(false);
    expect(isCounted(det({ s: 0, e: 10, confidence: undefined }))).toBe(true); // user-entered rows carry none
  });
});

describe("areaMinutes", () => {
  it("merges overlapping sources per area and clips to the window", () => {
    const items = [
      det({ s: 60, e: 120 }),
      det({ s: 90, e: 150, source: "process" as Detection["source"] }), // overlaps: 60..150 = 90
      det({ s: 200, e: 230 }), // +30
      det({ s: -30, e: 30, area: "skill" }), // clipped to 0..30
      det({ s: 300, e: 400, area: "skill", status: "disputed" }),
    ];
    expect(areaMinutes(items, base, base + 24 * 3_600_000)).toEqual({ work: 120, skill: 30 });
  });
});

describe("applyFilters", () => {
  const items = [
    det({ s: 0, e: 10 }),
    det({ s: 10, e: 20, status: "superseded" }),
    det({ s: 20, e: 30, area: "skill", source: "process" as Detection["source"], status: "confirmed" }),
  ];
  it("hides corrections unless asked", () => {
    expect(applyFilters(items, NO_FILTERS)).toHaveLength(2);
    expect(applyFilters(items, { ...NO_FILTERS, showCorrections: true })).toHaveLength(3);
  });
  it("filters by source, status and area", () => {
    expect(applyFilters(items, { ...NO_FILTERS, sources: new Set(["process"]) }).map((d) => d.area)).toEqual(["skill"]);
    expect(applyFilters(items, { ...NO_FILTERS, statuses: new Set(["detected"]) })).toHaveLength(1);
    expect(applyFilters(items, { ...NO_FILTERS, areas: new Set(["work"]) })).toHaveLength(1);
  });
  it("bulk confirm only touches unreviewed rows", () => {
    expect(confirmable(items, new Set(items.map((d) => d.id)))).toEqual([items[0]!.id]);
  });
});

describe("visibleHours", () => {
  it("covers the items and the day start, at least 8 hours", () => {
    expect(visibleHours([], 6, null)).toEqual([6, 16]);
    expect(visibleHours([{ start: 5 * 60 + 30, end: 23 * 60 + 10 }], 6, null)).toEqual([4, 24]);
    expect(visibleHours([{ start: 9 * 60, end: 19 * 60 }], 4, null)).toEqual([8, 19]); // no empty early hours
    expect(visibleHours([{ start: 9 * 60, end: 10 * 60 }], 4, null)).toEqual([8, 16]); // at least 8 hours
    expect(visibleHours([], 20, null)).toEqual([16, 24]);
    expect(visibleHours([], 6, 19 * 60 + 5)).toEqual([6, 21]);
  });
});
