import type { CalendarEntry } from "@/api/types";
import { addDays, dayOf, dayStartMs, minutesInto, type Day } from "./days";

export type EntryKind = CalendarEntry["kind"];

export function entryKey(e: CalendarEntry): string {
  return `${e.kind}:${e.id}:${e.occurs_on ?? e.starts_at}`;
}

/** A one-off task's due time: a point, not a block. */
export function isPoint(e: CalendarEntry): boolean {
  return !e.all_day && e.starts_at === e.ends_at;
}

/** The days an entry shows on (all-day entries: their days; timed entries: every day they touch). */
export function daysOf(e: CalendarEntry, tz: string): Day[] {
  if (e.all_day && e.occurs_on) return [e.occurs_on];
  const s = Date.parse(e.starts_at);
  const en = Math.max(s, Date.parse(e.ends_at) - 1);
  const first = dayOf(s, tz);
  const last = dayOf(en, tz);
  const out: Day[] = [];
  for (let d = first; d <= last && out.length < 62; d = addDays(d, 1)) out.push(d);
  return out;
}

export interface DayBucket {
  allDay: CalendarEntry[];
  timed: { entry: CalendarEntry; start: number; end: number }[];
}

/** Groups entries by visible day; timed entries carry their minutes within that day. */
export function bucketByDay(entries: CalendarEntry[], days: Day[], tz: string): Map<Day, DayBucket> {
  const map = new Map<Day, DayBucket>(days.map((d) => [d, { allDay: [], timed: [] }]));
  for (const e of entries) {
    for (const d of daysOf(e, tz)) {
      const b = map.get(d);
      if (!b) continue;
      if (e.all_day) {
        b.allDay.push(e);
      } else {
        const s = Date.parse(e.starts_at);
        const en = Date.parse(e.ends_at);
        b.timed.push({ entry: e, start: minutesInto(s, d, tz), end: minutesInto(en, d, tz) });
      }
    }
  }
  return map;
}

/** Splits a long window into ≤ 62-day pieces (the /calendar limit). */
export function windows(from: Day, to: Day, maxDays = 62): [Day, Day][] {
  const out: [Day, Day][] = [];
  for (let s = from; s < to; s = addDays(s, maxDays)) {
    const e = addDays(s, maxDays);
    out.push([s, e < to ? e : to]);
  }
  return out;
}

export function windowInstants(from: Day, to: Day, tz: string): { from: string; to: string } {
  return { from: new Date(dayStartMs(from, tz)).toISOString(), to: new Date(dayStartMs(to, tz)).toISOString() };
}

export const KIND_LABEL: Record<EntryKind, string> = {
  event: "Event",
  task_occurrence: "Task",
  entertainment_slot: "Entertainment",
  skill_session: "Skill session",
};
