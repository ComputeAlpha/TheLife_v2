import type { CalendarEntry } from "@/api/types";
import { dayOf, fmtDay, range, type Day } from "@/lib/calendar/days";
import { entryKey, isPoint } from "@/lib/calendar/entries";
import { areaMeta } from "@/lib/areas";
import { timeRange } from "@/lib/time";
import { cn } from "@/ui/cn";
import { EmptyState } from "@/ui/States";
import { KIND_ICON, areaVars, isDone, isRecurring, statusLabel } from "./EntryChip";
import { entriesOfDay } from "./MonthView";
import { Repeat } from "lucide-react";

export function AgendaView({ from, days, entries, tz, now, onOpen }: { from: Day; days: number; entries: CalendarEntry[]; tz: string; now: number; onOpen: (e: CalendarEntry) => void }) {
  const today = dayOf(now, tz);
  const list = range(from, days)
    .map((d) => ({ d, items: entriesOfDay(entries, d, tz) }))
    .filter((x) => x.items.length);
  if (!list.length) return <EmptyState title="Nothing planned" body="Double-click a day in the month view, or use + New, to plan something." />;
  return (
    <div className="mx-auto max-w-3xl space-y-6 p-4">
      {list.map(({ d, items }) => (
        <section key={d} aria-label={fmtDay(d, "EEEE d MMMM")} className="grid grid-cols-[72px_1fr] gap-4">
          <div className="pt-1 text-right">
            <p className={cn("text-[11px] font-medium uppercase tracking-wider", d === today ? "text-accent" : "text-text-3")}>{fmtDay(d, "EEE")}</p>
            <p className={cn("font-display text-2xl leading-tight", d === today ? "text-accent" : "text-text")}>{Number(d.slice(8))}</p>
            <p className="text-[11px] text-text-3">{fmtDay(d, "MMM")}</p>
          </div>
          <ul className="space-y-1.5">
            {items.map((e) => {
              const Icon = KIND_ICON[e.kind];
              const s = statusLabel(e);
              return (
                <li key={entryKey(e)}>
                  <button
                    onClick={() => onOpen(e)}
                    style={areaVars(e.area)}
                    className="flex w-full items-center gap-3 rounded-xl border border-line px-3 py-2.5 text-left transition-colors hover:border-[color-mix(in_oklab,var(--a)_50%,transparent)] hover:bg-[color-mix(in_oklab,var(--a)_6%,transparent)]"
                  >
                    <span className="h-9 w-1 shrink-0 rounded-full bg-[var(--a)]" aria-hidden />
                    <span className="w-24 shrink-0 tabular text-[13px] text-text-2">
                      {e.all_day ? "All day" : isPoint(e) ? `Due ${timeRange(e.starts_at, e.starts_at, tz)}` : timeRange(e.starts_at, e.ends_at, tz)}
                    </span>
                    <span className="min-w-0 flex-1">
                      <span className={cn("block truncate text-sm text-text", isDone(e) && "line-through decoration-text-3 text-text-2")}>{e.title}</span>
                      <span className="flex items-center gap-1.5 text-[12px] text-text-3">
                        <Icon className="size-3" aria-hidden /> {areaMeta(e.area).label}
                        {isRecurring(e) ? <Repeat className="size-3" aria-label="Repeats" /> : null}
                        {s ? <span>· {s}</span> : null}
                      </span>
                    </span>
                  </button>
                </li>
              );
            })}
          </ul>
        </section>
      ))}
    </div>
  );
}
