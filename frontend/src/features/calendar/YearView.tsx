import { useMemo } from "react";
import type { CalendarEntry, Score } from "@/api/types";
import { addDays, addMonths, dayOf, daysInMonth, fmtDay, startOfWeek, weekdayIndex, type Day } from "@/lib/calendar/days";
import { daysOf } from "@/lib/calendar/entries";
import { cn } from "@/ui/cn";
import { scoreColor } from "./DayHeader";

export type YearMetric = "planned" | "score";

/** Twelve mini months; each day shaded by how much was planned, or coloured by its score. */
export function YearView({ year, entries, scores, tz, now, metric, onDay }: {
  year: Day; // YYYY-01-01
  entries: CalendarEntry[];
  scores: Map<Day, Score>;
  tz: string;
  now: number;
  metric: YearMetric;
  onDay: (d: Day) => void;
}) {
  const counts = useMemo(() => {
    const m = new Map<Day, number>();
    for (const e of entries) for (const d of daysOf(e, tz)) m.set(d, (m.get(d) ?? 0) + 1);
    return m;
  }, [entries, tz]);
  const max = Math.max(1, ...counts.values());
  const today = dayOf(now, tz);
  const months = Array.from({ length: 12 }, (_, i) => addMonths(year, i));

  return (
    <div className="grid gap-x-6 gap-y-8 p-4 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4">
      {months.map((m) => {
        const first = startOfWeek(m);
        const cells = weekdayIndex(m) + daysInMonth(m);
        const total = Math.ceil(cells / 7) * 7;
        return (
          <section key={m} aria-label={fmtDay(m, "MMMM yyyy")}>
            <h3 className="mb-2 font-display text-[15px] font-semibold">{fmtDay(m, "MMMM")}</h3>
            <div className="grid grid-cols-7 gap-1">
              {["M", "T", "W", "T", "F", "S", "S"].map((w, i) => (
                <span key={i} className="text-center text-[10px] text-text-3">{w}</span>
              ))}
              {Array.from({ length: total }, (_, i) => {
                const d = addDays(first, i);
                if (!d.startsWith(m.slice(0, 7))) return <span key={d} />;
                const n = counts.get(d) ?? 0;
                const s = scores.get(d);
                // Fill strength skips 0.53–0.69, where neither digit colour reaches 4.5:1 on cyan over black.
                const t = n / max;
                const alpha = t <= 0.5 ? 0.12 + 0.8 * t : 0.7 + 0.4 * (t - 0.5);
                const bg =
                  metric === "score"
                    ? s?.final != null ? scoreColor(s.final) : undefined
                    : n ? `rgb(34 211 238 / ${alpha})` : undefined;
                // Dark digits on bright fills, light digits on dim ones (contrast ≥ 4.5:1 either way).
                const darkText = metric === "score" ? s?.final != null : n > 0 && alpha >= 0.7;
                return (
                  <button
                    key={d}
                    onClick={() => onDay(d)}
                    title={`${fmtDay(d, "EEE d MMM")}: ${n} planned${s?.final != null ? `, score ${Math.round(s.final)}` : ""}`}
                    className={cn(
                      "grid aspect-square place-items-center rounded-md text-[11px] tabular ring-1 ring-line/60 transition-transform hover:scale-110",
                      d === today && "ring-2 ring-accent",
                      darkText ? "font-semibold text-accent-ink" : bg || d === today ? "text-text" : "text-text-2 hover:text-text",
                    )}
                    style={bg ? { background: bg } : undefined}
                  >
                    {Number(d.slice(8))}
                  </button>
                );
              })}
            </div>
          </section>
        );
      })}
    </div>
  );
}
