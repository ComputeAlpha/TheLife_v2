import { fmt, localDay, relative } from "./time";

describe("time helpers", () => {
  it("formats in the user's zone, not the browser's", () => {
    expect(fmt("2026-11-16T20:00:00Z", "Asia/Kolkata", "yyyy-MM-dd HH:mm")).toBe("2026-11-17 01:30");
    expect(fmt("2026-11-16T20:00:00Z", "America/New_York", "yyyy-MM-dd HH:mm")).toBe("2026-11-16 15:00");
  });
  it("honours the day start hour for the local day", () => {
    // 02:30 in Kolkata with days starting at 04:00 still belongs to the 16th.
    expect(localDay("2026-11-16T21:00:00Z", "Asia/Kolkata", 4)).toBe("2026-11-16");
    expect(localDay("2026-11-16T21:00:00Z", "Asia/Kolkata", 0)).toBe("2026-11-17");
    expect(localDay("2026-11-16T23:00:00Z", "Asia/Kolkata", 4)).toBe("2026-11-17");
  });
  it("gives short relative times", () => {
    const now = Date.parse("2026-11-16T12:00:00Z");
    expect(relative("2026-11-16T12:05:00Z", now)).toBe("in 5 min");
    expect(relative("2026-11-16T09:00:00Z", now)).toBe("3 h ago");
    expect(relative("2026-11-16T12:00:10Z", now)).toBe("just now");
  });
});

import { fromLocalInput, toLocalInput } from "./time";
describe("datetime-local in the user's zone", () => {
  it("round-trips", () => {
    expect(toLocalInput("2026-11-16T04:30:00Z", "Asia/Kolkata")).toBe("2026-11-16T10:00");
    expect(fromLocalInput("2026-11-16T10:00", "Asia/Kolkata")).toBe("2026-11-16T04:30:00.000Z");
    expect(fromLocalInput("2026-11-16T10", "Asia/Kolkata")).toBeNull();
  });
});
