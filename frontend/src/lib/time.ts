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

/** "YYYY-MM-DDTHH:mm" for <input type="datetime-local">, in the user's zone. */
export function toLocalInput(value: string | number | Date, tz: string): string {
  return format(inZone(value, tz), "yyyy-MM-dd'T'HH:mm");
}
/** The instant (ISO, UTC) of a datetime-local value read in the user's zone; null when incomplete. */
export function fromLocalInput(value: string, tz: string): string | null {
  const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})$/.exec(value);
  if (!m) return null;
  const [, y, mo, d, h, mi] = m.map(Number) as number[];
  return new Date(new TZDate(y!, mo! - 1, d!, h!, mi!, 0, tz).getTime()).toISOString();
}
/** "09:30" or "09:30–10:15" in the zone. */
export function timeRange(startIso: string, endIso: string, tz: string): string {
  const s = fmt(startIso, tz, "HH:mm");
  return startIso === endIso ? s : `${s}–${fmt(endIso, tz, "HH:mm")}`;
}
export function minutesLabel(mins: number): string {
  const m = Math.round(mins);
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  const r = m % 60;
  return r ? `${h} h ${r} min` : `${h} h`;
}
