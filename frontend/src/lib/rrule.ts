import { RRule } from "rrule";
import { weekdayCode, type Day } from "./calendar/days";

export type Preset = "none" | "daily" | "weekdays" | "weekly" | "monthly" | "yearly" | "custom";

/** The rule for a preset, anchored on the day it starts. */
export function presetRule(p: Preset, day: Day): string | null {
  switch (p) {
    case "none": return null;
    case "daily": return "FREQ=DAILY";
    case "weekdays": return "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR";
    case "weekly": return `FREQ=WEEKLY;BYDAY=${weekdayCode(day)}`;
    case "monthly": return `FREQ=MONTHLY;BYMONTHDAY=${Number(day.slice(8))}`;
    case "yearly": return "FREQ=YEARLY";
    case "custom": return null;
  }
}

export function presetOf(rule: string | null | undefined, day: Day): Preset {
  if (!rule) return "none";
  const r = rule.replace(/^RRULE:/, "");
  for (const p of ["daily", "weekdays", "weekly", "monthly", "yearly"] as const) if (presetRule(p, day) === r) return p;
  return "custom";
}

/** "every weekday", "every 2 weeks on Monday"; null for an unreadable rule. */
export function describeRule(rule: string | null | undefined): string | null {
  if (!rule) return null;
  try {
    return RRule.fromString(rule.replace(/^RRULE:/, "")).toText();
  } catch {
    return null;
  }
}

/** Same shape check as the contract's pattern. */
export function ruleLooksValid(rule: string): boolean {
  return /^(RRULE:)?FREQ=(DAILY|WEEKLY|MONTHLY|YEARLY)(;[A-Z]+=[A-Z0-9,+-]+)*$/.test(rule);
}
