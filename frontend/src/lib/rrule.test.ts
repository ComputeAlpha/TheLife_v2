import { describeRule, presetOf, presetRule, ruleLooksValid } from "./rrule";

describe("recurrence presets", () => {
  it("round-trips presets anchored on the start day", () => {
    expect(presetRule("weekly", "2026-11-18")).toBe("FREQ=WEEKLY;BYDAY=WE");
    expect(presetOf("FREQ=WEEKLY;BYDAY=WE", "2026-11-18")).toBe("weekly");
    expect(presetOf("FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR", "2026-11-18")).toBe("weekdays");
    expect(presetOf("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO", "2026-11-18")).toBe("custom");
    expect(presetOf(null, "2026-11-18")).toBe("none");
  });
  it("describes rules in words", () => {
    expect(describeRule("FREQ=DAILY")).toBe("every day");
    expect(describeRule("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO")).toBe("every 2 weeks on Monday");
    expect(ruleLooksValid("FREQ=HOURLY")).toBe(false);
  });
});
