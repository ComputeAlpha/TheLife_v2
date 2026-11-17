// Calendar days as "YYYY-MM-DD" strings. Day arithmetic happens on plain dates (UTC, no DST); only the conversion
// to instants needs the user's timezone.
import { TZDate } from "@date-fns/tz";
import { format } from "date-fns";

export type Day = string;

function parse(day: Day): Date {
  const [y, m, d] = day.split("-").map(Number);
  return new Date(Date.UTC(y!, m! - 1, d!));
}
function str(d: Date): Day {
  return d.toISOString().slice(0, 10);
}

export function addDays(day: Day, n: number): Day {
  const d = parse(day);
  d.setUTCDate(d.getUTCDate() + n);
  return str(d);
}
export function addMonths(day: Day, n: number): Day {
  const d = parse(day);
  const target = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + n, 1));
  const last = new Date(Date.UTC(target.getUTCFullYear(), target.getUTCMonth() + 1, 0)).getUTCDate();
  target.setUTCDate(Math.min(d.getUTCDate(), last));
  return str(target);
}
export function diffDays(a: Day, b: Day): number {
  return Math.round((parse(a).getTime() - parse(b).getTime()) / 86_400_000);
}
/** 0 = Monday … 6 = Sunday. */
export function weekdayIndex(day: Day): number {
  return (parse(day).getUTCDay() + 6) % 7;
}
export const WEEKDAY_CODES = ["MO", "TU", "WE", "TH", "FR", "SA", "SU"] as const;
export function weekdayCode(day: Day): (typeof WEEKDAY_CODES)[number] {
  return WEEKDAY_CODES[weekdayIndex(day)]!;
}
export function startOfWeek(day: Day): Day {
  return addDays(day, -weekdayIndex(day));
}
export function startOfMonth(day: Day): Day {
  return day.slice(0, 8) + "01";
}
export function startOfYear(day: Day): Day {
  return day.slice(0, 4) + "-01-01";
}
export function daysInMonth(day: Day): number {
  const d = parse(day);
  return new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + 1, 0)).getUTCDate();
}
export function range(from: Day, count: number): Day[] {
  return Array.from({ length: count }, (_, i) => addDays(from, i));
}
/** The 6×7 grid shown for a month: Monday-first weeks covering the month. */
export function monthGrid(day: Day): Day[] {
  return range(startOfWeek(startOfMonth(day)), 42);
}
export function isValidDay(s: string | undefined): s is Day {
  return !!s && /^\d{4}-\d{2}-\d{2}$/.test(s) && str(parse(s)) === s;
}

/** Local midnight of the day in the zone, as an instant (ms). */
export function dayStartMs(day: Day, tz: string): number {
  const [y, m, d] = day.split("-").map(Number);
  return new TZDate(y!, m! - 1, d!, 0, 0, 0, tz).getTime();
}
/** The calendar day (local midnight boundaries) an instant falls on. */
export function dayOf(ms: number, tz: string): Day {
  return format(new TZDate(ms, tz), "yyyy-MM-dd");
}
/** Minutes since local midnight of `day` (can exceed 1440 or go negative for other days; DST-correct). */
export function minutesInto(ms: number, day: Day, tz: string): number {
  return (ms - dayStartMs(day, tz)) / 60_000;
}
/** Formats a day string (interpreted as a plain date) with a date-fns pattern. */
export function fmtDay(day: Day, pattern: string): string {
  const [y, m, d] = day.split("-").map(Number);
  return format(new Date(y!, m! - 1, d!), pattern);
}
