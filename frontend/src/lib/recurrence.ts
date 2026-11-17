// The recurrence builder's model (frontend_lld.md §6.9): frequency, interval, weekdays, end; and a preview.
import { RRule } from "rrule";
import { WEEKDAY_CODES, type Day } from "./calendar/days";

export type Freq = "DAILY" | "WEEKLY" | "MONTHLY" | "YEARLY";
export type Weekday = (typeof WEEKDAY_CODES)[number];
export interface RuleParts {
  freq: Freq;
  interval: number;
  byday: Weekday[]; // weekly only
  end: { kind: "never" } | { kind: "until"; day: Day } | { kind: "count"; n: number };
}

/** Parts for a rule string, or null when the rule uses features the builder doesn't model (edit it as text). */
export function parseRule(rule: string | null | undefined): RuleParts | null {
  if (!rule) return null;
  const kv = new Map(rule.replace(/^RRULE:/, "").split(";").map((p) => p.split("=") as [string, string]));
  const freq = kv.get("FREQ") as Freq | undefined;
  if (!freq || !["DAILY", "WEEKLY", "MONTHLY", "YEARLY"].includes(freq)) return null;
  for (const k of kv.keys()) if (!["FREQ", "INTERVAL", "BYDAY", "UNTIL", "COUNT", "BYMONTHDAY"].includes(k)) return null;
  if (kv.has("BYDAY") && freq !== "WEEKLY") return null;
  const byday = (kv.get("BYDAY")?.split(",") ?? []) as Weekday[];
  if (byday.some((d) => !WEEKDAY_CODES.includes(d))) return null;
  const until = kv.get("UNTIL");
  const count = kv.get("COUNT");
  return {
    freq,
    interval: Math.max(1, Number(kv.get("INTERVAL") ?? 1) || 1),
    byday,
    end: until ? { kind: "until", day: `${until.slice(0, 4)}-${until.slice(4, 6)}-${until.slice(6, 8)}` } : count ? { kind: "count", n: Number(count) } : { kind: "never" },
  };
}

/** The rule for parts. Monthly rules repeat on the start day's date (the server anchors on rrule_dtstart). */
export function buildRule(p: RuleParts): string {
  const out = [`FREQ=${p.freq}`];
  if (p.interval > 1) out.push(`INTERVAL=${p.interval}`);
  if (p.freq === "WEEKLY" && p.byday.length) out.push(`BYDAY=${WEEKDAY_CODES.filter((d) => p.byday.includes(d)).join(",")}`);
  if (p.end.kind === "until") out.push(`UNTIL=${p.end.day.replace(/-/g, "")}T235959Z`);
  if (p.end.kind === "count") out.push(`COUNT=${Math.max(1, Math.floor(p.end.n))}`);
  return out.join(";");
}

/**
 * The next `n` days the rule falls on, from `startDay` (wall-clock dates; the rule runs on floating time so the
 * zone doesn't shift days), at or after `fromDay`.
 */
export function nextDays(rule: string, startDay: Day, fromDay: Day, n = 5): Day[] {
  try {
    const [y, m, d] = startDay.split("-").map(Number) as [number, number, number];
    const r = RRule.fromString(rule.replace(/^RRULE:/, ""));
    const rr = new RRule({ ...r.origOptions, dtstart: new Date(Date.UTC(y, m - 1, d, 12)) });
    const [fy, fm, fd] = fromDay.split("-").map(Number) as [number, number, number];
    const out: Day[] = [];
    let after = new Date(Date.UTC(fy, fm - 1, fd, 0));
    for (let i = 0; i < n; i++) {
      const next = rr.after(after, i === 0);
      if (!next) break;
      out.push(next.toISOString().slice(0, 10));
      after = next;
    }
    return out;
  } catch {
    return [];
  }
}
