import * as P from "@radix-ui/react-popover";
import { useMemo } from "react";
import type { CalendarEntry, DayProfileOverride, Score } from "@/api/types";
import { addDays, dayOf, dayStartMs, fmtDay, monthGrid, weekdayCode, type Day } from "@/lib/calendar/days";
import { bucketByDay, entryKey } from "@/lib/calendar/entries";
import { cn } from "@/ui/cn";
import { EntryChip } from "./EntryChip";
import { ProfileBadge, ScoreDot } from "./DayHeader";
import type { CreateDraft } from "./QuickCreate";

const MAX_CHIPS = 3;

function sortForDay(a: CalendarEntry, b: CalendarEntry) {
  if (a.all_day !== b.all_day) return a.all_day ? -1 : 1;
  return a.starts_at.localeCompare(b.starts_at);
}

export function MonthView({ day, entries, tz, now, workdays, scores, overrides, onOpen, onDay, onCreate }: {
  day: Day;
  entries: CalendarEntry[];
  tz: string;
  now: number;
  workdays: string[];
  scores: Map<Day, Score>;
  overrides: Map<Day, DayProfileOverride>;
  onOpen: (e: CalendarEntry) => void;
  onDay: (d: Day) => void;
  onCreate: (d: CreateDraft) => void;
}) {
  const grid = monthGrid(day);
  const buckets = useMemo(() => bucketByDay(entries, grid, tz), [entries, grid, tz]);
  const today = dayOf(now, tz);
  const month = day.slice(0, 7);
  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="grid grid-cols-7 border-b border-line">
        {grid.slice(0, 7).map((d) => (
          <div key={d} className="px-2 py-2 text-[11px] font-medium uppercase tracking-wider text-text-3">{fmtDay(d, "EEE")}</div>
        ))}
      </div>
      <div className="grid min-h-0 flex-1 auto-rows-[minmax(7.5rem,1fr)] grid-cols-7 overflow-y-auto">
        {grid.map((d) => {
          const b = buckets.get(d)!;
          const list = [...b.allDay, ...b.timed.map((t) => t.entry)].sort(sortForDay);
          const inMonth = d.startsWith(month);
          const isToday = d === today;
          const off = !workdays.includes(weekdayCode(d));
          return (
            <div
              key={d}
              onDoubleClick={() => onCreate({ start: dayStartMs(d, tz) + 9 * 3_600_000, end: dayStartMs(d, tz) + 10 * 3_600_000 })}
              className={cn(
                "group/cell flex min-h-[7.5rem] min-w-0 flex-col gap-1 border-b border-l border-line p-1.5 first:border-l-0 [&:nth-child(7n+1)]:border-l-0",
                !inMonth && "opacity-45",
                off && "bg-white/[.012]",
                isToday && "relative ring-1 ring-inset ring-accent/60",
              )}
            >
              <div className="flex items-center gap-1">
                <button
                  onClick={() => onDay(d)}
                  className={cn(
                    "grid h-6 min-w-6 place-items-center rounded-full px-1 text-[13px] tabular hover:text-accent",
                    isToday ? "bg-accent font-semibold text-accent-ink hover:text-accent-ink" : "text-text-2",
                  )}
                >
                  <span className="sr-only">{fmtDay(d, "EEEE")} </span>
                  {Number(d.slice(8))}
                  <span className="sr-only"> {fmtDay(d, "MMMM")}</span>
                </button>
                <ProfileBadge override={overrides.get(d)} />
                <span className="ml-auto flex items-center gap-1.5">
                  {list.length > MAX_CHIPS ? (
                    <P.Root>
                      <P.Trigger className="rounded-full border border-line px-1.5 text-[11px] font-medium leading-4 text-text-2 hover:border-accent/50 hover:text-accent">
                        +{list.length - MAX_CHIPS} more
                      </P.Trigger>
                      <P.Portal>
                        <P.Content sideOffset={4} align="end" className="z-50 max-h-[60vh] w-72 space-y-1 overflow-y-auto rounded-xl border border-line-strong bg-bg p-3 shadow-2xl">
                          <p className="mb-2 font-display text-sm font-semibold">{fmtDay(d, "EEEE d MMMM")}</p>
                          {list.map((e) => <EntryChip key={entryKey(e)} entry={e} tz={tz} onClick={() => onOpen(e)} />)}
                        </P.Content>
                      </P.Portal>
                    </P.Root>
                  ) : null}
                  <ScoreDot score={scores.get(d)} />
                </span>
              </div>
              <div className="min-h-0 min-w-0 flex-1 space-y-0.5 overflow-hidden">
                {list.slice(0, MAX_CHIPS).map((e) => <EntryChip key={entryKey(e)} entry={e} tz={tz} showTime={false} onClick={() => onOpen(e)} />)}
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}

/** The day's entries, ordered, for lists (agenda, Today). */
export function entriesOfDay(entries: CalendarEntry[], d: Day, tz: string): CalendarEntry[] {
  const b = bucketByDay(entries, [d], tz).get(d)!;
  return [...b.allDay, ...b.timed.map((t) => t.entry)].sort(sortForDay);
}

export { addDays };
