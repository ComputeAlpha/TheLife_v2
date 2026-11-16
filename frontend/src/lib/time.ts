// Dates are always shown in the user's timezone from /me, never the browser's (frontend_lld.md §5).
import { TZDate } from "@date-fns/tz";
import { addHours, format } from "date-fns";

export function inZone(value: string | number | Date, tz: string): TZDate {
  return new TZDate(typeof value === "string" ? Date.parse(value) : +value, tz);
}

export function nowIn(tz: string): TZDate {
  return TZDate.tz(tz);
}

/** Formats an instant in the zone with a date-fns pattern. */
export function fmt(value: string | number | Date, tz: string, pattern: string): string {
  return format(inZone(value, tz), pattern);
}

/**
 * The user's local day (a LocalDate, "YYYY-MM-DD") for an instant: days start at day_start_hour, so 02:00 with a
 * day start of 4 still belongs to the previous day.
 */
export function localDay(value: string | number | Date, tz: string, dayStartHour: number): string {
  return format(addHours(inZone(value, tz), -dayStartHour), "yyyy-MM-dd");
}

export function greeting(tz: string, now: number): string {
  const h = inZone(now, tz).getHours();
  if (h < 5) return "Still up";
  if (h < 12) return "Good morning";
  if (h < 17) return "Good afternoon";
  return "Good evening";
}

/** "in 5 min", "3 h ago": short relative time. */
export function relative(value: string | number | Date, now: number = Date.now()): string {
  const t = typeof value === "string" ? Date.parse(value) : +value;
  const s = Math.round((t - now) / 1000);
  const a = Math.abs(s);
  const unit = a < 60 ? `${a} s` : a < 3600 ? `${Math.round(a / 60)} min` : a < 86400 ? `${Math.round(a / 3600)} h` : `${Math.round(a / 86400)} d`;
  if (a < 30) return "just now";
  return s > 0 ? `in ${unit}` : `${unit} ago`;
}

export function timezones(): string[] {
  try {
    return Intl.supportedValuesOf("timeZone");
  } catch {
    return ["UTC"];
  }
}
