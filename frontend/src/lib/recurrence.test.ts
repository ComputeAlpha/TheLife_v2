import { describe, expect, it } from "vitest";
import { buildRule, nextDays, parseRule } from "./recurrence";

describe("recurrence builder", () => {
  it("round-trips what it models", () => {
    for (const r of ["FREQ=DAILY", "FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR", "FREQ=MONTHLY;COUNT=6", "FREQ=YEARLY;UNTIL=20271231T235959Z"]) {
      expect(buildRule(parseRule(r)!)).toBe(r);
    }
  });
  it("orders weekdays and leaves unmodelled rules to the text field", () => {
    expect(buildRule({ freq: "WEEKLY", interval: 1, byday: ["FR", "MO"], end: { kind: "never" } })).toBe("FREQ=WEEKLY;BYDAY=MO,FR");
    expect(parseRule("FREQ=MONTHLY;BYDAY=1MO")).toBeNull();
    expect(parseRule("FREQ=DAILY;BYHOUR=9")).toBeNull();
    expect(parseRule(null)).toBeNull();
    expect(parseRule("FREQ=YEARLY;UNTIL=20271231T235959Z")!.end).toEqual({ kind: "until", day: "2027-12-31" });
  });
  it("previews the next dates from a day", () => {
    expect(nextDays("FREQ=WEEKLY;BYDAY=MO,WE", "2026-11-02", "2026-11-17", 3)).toEqual(["2026-11-18", "2026-11-23", "2026-11-25"]);
    expect(nextDays("FREQ=DAILY;COUNT=3", "2026-11-16", "2026-11-17")).toEqual(["2026-11-17", "2026-11-18"]);
    expect(nextDays("FREQ=DAILY;INTERVAL=2", "2026-11-16", "2026-11-16", 3)).toEqual(["2026-11-16", "2026-11-18", "2026-11-20"]);
    expect(nextDays("garbage", "2026-11-16", "2026-11-16")).toEqual([]);
  });
});
