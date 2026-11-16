import type { CSSProperties } from "react";
import { BookOpen, Check, Gamepad2, Repeat, CalendarClock, CircleDashed, X } from "lucide-react";
import type { CalendarEntry } from "@/api/types";
import { areaMeta } from "@/lib/areas";
import { isPoint } from "@/lib/calendar/entries";
import { timeRange } from "@/lib/time";
import { cn } from "@/ui/cn";

export const KIND_ICON = {
  event: CalendarClock,
  task_occurrence: CircleDashed,
  entertainment_slot: Gamepad2,
  skill_session: BookOpen,
} as const;

/** Area colour as CSS variables the chip classes read (fill, edge, text). */
export function areaVars(area: string | null | undefined): CSSProperties {
  const c = areaMeta(area).color;
  return { "--a": c } as CSSProperties;
}

export function isDone(e: CalendarEntry) {
  return e.status === "done" || e.status === "consumed";
}
export function isRecurring(e: CalendarEntry) {
  return e.kind === "event" ? !!e.occurs_on : e.kind === "task_occurrence" && e.all_day;
}

export function statusLabel(e: CalendarEntry): string | null {
  switch (e.status) {
    case "done": return "Done";
    case "skipped": return "Skipped";
    case "missed": return "Missed";
    case "consumed": return "Enjoyed";
    default: return null;
  }
}

/** The compact, one-line chip used in all-day rows, month cells and lists. */
export function EntryChip({ entry, tz, showTime = true, className, onClick }: {
  entry: CalendarEntry;
  tz: string;
  showTime?: boolean;
  className?: string;
  onClick?: () => void;
}) {
  const Icon = KIND_ICON[entry.kind];
  const done = isDone(entry);
  const missed = entry.status === "missed";
  const skipped = entry.status === "skipped";
  return (
    <button
      type="button"
      onClick={(ev) => {
        ev.stopPropagation();
        onClick?.();
      }}
      style={areaVars(entry.area)}
      className={cn(
        "group/chip flex w-full min-w-0 items-center gap-1.5 rounded-md border-l-[3px] px-1.5 py-[3px] text-left text-[12px] leading-4 transition-colors",
        "border-l-[var(--a)] bg-[color-mix(in_oklab,var(--a)_14%,transparent)] hover:bg-[color-mix(in_oklab,var(--a)_24%,transparent)]",
        missed && "outline outline-1 -outline-offset-1 outline-alert/70",
        skipped && "border-dashed opacity-60",
        className,
      )}
      title={entry.title}
    >
      {entry.kind === "task_occurrence" ? (
        <span className={cn("grid size-3 shrink-0 place-items-center rounded-[3px] border", done ? "border-good bg-good text-black" : "border-[var(--a)]")}>
          {done ? <Check className="size-2.5" strokeWidth={3} /> : skipped ? <X className="size-2.5" /> : null}
        </span>
      ) : (
        <Icon className="size-3 shrink-0 text-[var(--a)]" aria-hidden />
      )}
      {showTime && !entry.all_day ? (
        <span className="shrink-0 tabular text-text-2">{timeRange(entry.starts_at, isPoint(entry) ? entry.starts_at : entry.starts_at, tz)}</span>
      ) : null}
      <span className={cn("truncate text-text", done && "text-text-2 line-through decoration-text-3")}>{entry.title}</span>
      {isRecurring(entry) ? <Repeat className="ml-auto size-3 shrink-0 text-text-3" aria-label="Repeats" /> : null}
    </button>
  );
}
