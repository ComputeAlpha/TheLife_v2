// Pure helpers for the Scores page (frontend_lld.md §6.7, scoring_lld.md §8–§10).
import { addDays, addMonths, fmtDay, startOfMonth, startOfWeek, type Day } from "./calendar/days";
import type { Period } from "@/api/scores";

export const WINDOW: Record<Period, number> = { day: 30, week: 12, month: 12 };

/** The period start that contains `day`. */
export function periodStart(period: Period, day: Day): Day {
  return period === "day" ? day : period === "week" ? startOfWeek(day) : startOfMonth(day);
}

export function stepPeriod(period: Period, start: Day, n: number): Day {
  return period === "day" ? addDays(start, n) : period === "week" ? addDays(start, 7 * n) : addMonths(start, n);
}

/** The `count` period starts ending with the one containing `end`, oldest first. */
export function periodStarts(period: Period, end: Day, count = WINDOW[period]): Day[] {
  const last = periodStart(period, end);
  return Array.from({ length: count }, (_, i) => stepPeriod(period, last, i - count + 1));
}

/** The last calendar day of the period starting at `start`. */
export function periodEnd(period: Period, start: Day): Day {
  return addDays(stepPeriod(period, start, 1), -1);
}

export function periodLabel(period: Period, start: Day, long = false): string {
  if (period === "day") return fmtDay(start, long ? "EEEE d MMMM yyyy" : "EEE d MMM");
  if (period === "week") {
    const end = periodEnd("week", start);
    return long ? `Week of ${fmtDay(start, "d MMM")} – ${fmtDay(end, "d MMM yyyy")}` : fmtDay(start, "d MMM");
  }
  return fmtDay(start, long ? "MMMM yyyy" : "MMM");
}

/** Points the adjustment moved the score: final = clamp(round(base + 0.3·pct)) (formula version 2). */
export function adjustmentPoints(pct: number | null | undefined): number {
  return Math.round(0.3 * (pct ?? 0) * 10) / 10;
}

/** Score band for colour: ≥ 75 good, ≥ 50 accent, else warn. Always shown with the number. */
export function band(v: number): "good" | "accent" | "warn" {
  return v >= 75 ? "good" : v >= 50 ? "accent" : "warn";
}
