import { useEffect, useLayoutEffect, useMemo, useRef, useState, type PointerEvent as RPointerEvent } from "react";
import { Repeat } from "lucide-react";
import type { CalendarEntry, DayProfileOverride, Detection, Score } from "@/api/types";
import { addDays, dayOf, dayStartMs, fmtDay, minutesInto, weekdayCode, type Day } from "@/lib/calendar/days";
import { bucketByDay, entryKey, isPoint } from "@/lib/calendar/entries";
import { layoutColumn } from "@/lib/calendar/layout";
import { areaMeta } from "@/lib/areas";
import { fmt, timeRange } from "@/lib/time";
import { cn } from "@/ui/cn";
import { EntryChip, KIND_ICON, areaVars, isDone, isRecurring } from "./EntryChip";
import { ProfileBadge, ProfileMenu, ScoreDot } from "./DayHeader";
import type { CreateDraft } from "./QuickCreate";

export const HOUR_PX = 52;
const PX = HOUR_PX / 60;
const SNAP = 15;
const snap = (m: number) => Math.round(m / SNAP) * SNAP;

export interface GridProps {
  days: Day[];
  entries: CalendarEntry[];
  tz: string;
  now: number;
  workdays: string[];
  dayStartHour: number;
  scores: Map<Day, Score>;
  overrides: Map<Day, DayProfileOverride>;
  profiles: string[];
  detections: Detection[] | null;
  onCreate: (d: CreateDraft) => void;
  onOpen: (e: CalendarEntry) => void;
  onMove: (e: CalendarEntry, startMs: number, endMs: number) => void;
  onDay: (d: Day) => void;
  onSetProfile: (d: Day, p: string | null) => void;
}

/** Entries the user can drag: single events, slots, sessions and one-off task due times. */
export function canDrag(e: CalendarEntry) {
  if (e.all_day) return false;
  if (e.kind === "event") return !isRecurring(e);
  return true;
}

type Drag = {
  entry: CalendarEntry;
  mode: "move" | "resize";
  x0: number;
  y0: number;
  day0: number;
  moved: boolean;
  dMin: number;
  dDay: number;
};
type Select = { day: number; a: number; b: number; moved: boolean };

export function TimeGrid(p: GridProps) {
  const { days, tz, now } = p;
  const scroller = useRef<HTMLDivElement>(null);
  const cols = useRef<HTMLDivElement>(null);
  const [drag, setDrag] = useState<Drag | null>(null);
  const [select, setSelect] = useState<Select | null>(null);
  const buckets = useMemo(() => bucketByDay(p.entries, days, tz), [p.entries, days, tz]);
  const today = dayOf(now, tz);
  const showActivity = !!p.detections;

  const activity = useMemo(() => {
    const m = new Map<Day, { d: Detection; start: number; end: number }[]>();
    for (const d of p.detections ?? []) {
      if (d.status === "superseded" || d.status === "disputed") continue;
      const s = Date.parse(d.started_at);
      const e = Date.parse(d.ended_at);
      for (const day of days) {
        const a = minutesInto(s, day, tz);
        const b = minutesInto(e, day, tz);
        if (b > 0 && a < 1440) (m.get(day) ?? m.set(day, []).get(day)!).push({ d, start: Math.max(0, a), end: Math.min(1440, b) });
      }
    }
    return m;
  }, [p.detections, days, tz]);

  // Open on the useful part of the day: an hour before now, or the day start.
  useLayoutEffect(() => {
    const el = scroller.current;
    if (!el) return;
    const focusMin = days.includes(today) ? minutesInto(Date.now(), today, tz) - 60 : Math.max(7, p.dayStartHour + 3) * 60;
    el.scrollTop = Math.max(0, focusMin * PX);
    // only when the visible range changes
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [days.join()]);

  const colIndexAt = (clientX: number) => {
    const r = cols.current!.getBoundingClientRect();
    return Math.max(0, Math.min(days.length - 1, Math.floor(((clientX - r.left) / r.width) * days.length)));
  };
  const minuteAt = (clientY: number) => {
    const r = cols.current!.getBoundingClientRect();
    return Math.max(0, Math.min(1440, (clientY - r.top) / PX));
  };

  // ---- drag to create
  const onColumnDown = (ev: RPointerEvent<HTMLDivElement>, i: number) => {
    if (ev.button !== 0) return;
    (ev.currentTarget as HTMLElement).setPointerCapture(ev.pointerId);
    const m = Math.floor(minuteAt(ev.clientY) / SNAP) * SNAP;
    setSelect({ day: i, a: m, b: m + SNAP, moved: false });
  };
  const onColumnMove = (ev: RPointerEvent<HTMLDivElement>) => {
    if (!select) return;
    const m = snap(minuteAt(ev.clientY));
    if (m !== select.b) setSelect({ ...select, b: m, moved: true });
  };
  const onColumnUp = () => {
    if (!select) return;
    let a = Math.min(select.a, select.b);
    let b = Math.max(select.a, select.b);
    if (!select.moved || b - a < SNAP) {
      a = select.a;
      b = Math.min(1440, a + 60);
    }
    const base = dayStartMs(days[select.day]!, tz);
    setSelect(null);
    p.onCreate({ start: base + a * 60_000, end: base + b * 60_000 });
  };

  // ---- drag to move / resize
  const onEntryDown = (ev: RPointerEvent<HTMLElement>, entry: CalendarEntry, mode: "move" | "resize", dayIdx: number) => {
    ev.stopPropagation();
    if (ev.button !== 0) return;
    (ev.currentTarget as HTMLElement).setPointerCapture(ev.pointerId);
    setDrag({ entry, mode, x0: ev.clientX, y0: ev.clientY, day0: dayIdx, moved: false, dMin: 0, dDay: 0 });
  };
  const onEntryMove = (ev: RPointerEvent<HTMLElement>) => {
    if (!drag) return;
    const dx = ev.clientX - drag.x0;
    const dy = ev.clientY - drag.y0;
    if (!drag.moved && Math.hypot(dx, dy) < 5) return;
    if (!canDrag(drag.entry) || (drag.mode === "resize" && isPoint(drag.entry))) {
      if (!drag.moved) setDrag({ ...drag, moved: true });
      return;
    }
    const dMin = snap(dy / PX);
    const dDay = drag.mode === "move" ? colIndexAt(ev.clientX) - drag.day0 : 0;
    if (!drag.moved || dMin !== drag.dMin || dDay !== drag.dDay) setDrag({ ...drag, moved: true, dMin, dDay });
  };
  const onEntryUp = () => {
    if (!drag) return;
    const d = drag;
    setDrag(null);
    if (!d.moved) return p.onOpen(d.entry);
    if (!canDrag(d.entry) || (d.dMin === 0 && d.dDay === 0)) return;
    const s = Date.parse(d.entry.starts_at);
    const e = Date.parse(d.entry.ends_at);
    const shift = d.dMin * 60_000 + d.dDay * 86_400_000;
    if (d.mode === "move") p.onMove(d.entry, s + shift, e + shift);
    else p.onMove(d.entry, s, Math.max(s + SNAP * 60_000, e + d.dMin * 60_000));
  };
  const onKey = (ev: React.KeyboardEvent, entry: CalendarEntry) => {
    if (ev.key === "Enter" || ev.key === " ") {
      ev.preventDefault();
      p.onOpen(entry);
    }
  };

  // Escape cancels a drag in progress.
  useEffect(() => {
    const k = (e: KeyboardEvent) => e.key === "Escape" && (setDrag(null), setSelect(null));
    window.addEventListener("keydown", k);
    return () => window.removeEventListener("keydown", k);
  }, []);

  const gridCols = { gridTemplateColumns: `56px repeat(${days.length}, minmax(0, 1fr))` };
  const nowMin = minutesInto(now, today, tz);

  return (
    <div className="flex h-full min-h-0 flex-col">
      {/* Hours */}
      <div ref={scroller} className="relative min-h-0 flex-1 overflow-y-auto overscroll-contain">
        {/* Header and all-day rows live in the scroller (sticky), so their columns share the hours' width exactly. */}
        <div className="sticky top-0 z-40 bg-bg">
      {/* Day headers */}
          <div className="grid border-b border-line" style={gridCols}>
            <div className="flex items-end justify-center pb-2 text-[10.5px] text-text-3">{fmt(now, tz, "O")}</div>
            {days.map((d) => {
              const isToday = d === today;
              const off = !p.workdays.includes(weekdayCode(d));
              return (
                <div key={d} data-head={d} className={cn("group/h flex min-w-0 flex-col gap-1 border-l border-line px-2 py-2", off && "bg-white/[.012]")}>
                  <div className="flex items-center gap-1.5">
                    <button
                      onClick={() => p.onDay(d)}
                      className="flex min-w-0 items-baseline gap-1.5 rounded-md text-left hover:text-accent"
                    >
                      <span className="sr-only">{fmtDay(d, "EEEE d MMMM")}</span>
                      <span aria-hidden className={cn("text-[11px] font-medium uppercase tracking-wider", isToday ? "text-accent" : "text-text-3")}>{fmtDay(d, "EEE")}</span>
                      <span aria-hidden className={cn(
                        "grid h-7 min-w-7 place-items-center rounded-full px-1 font-display text-lg leading-none tabular",
                        isToday ? "bg-accent text-accent-ink shadow-[0_0_16px_rgb(34_211_238/.6)]" : "text-text",
                      )}>
                        {Number(d.slice(8))}
                      </span>
                    </button>
                    <span className="ml-auto flex items-center gap-1">
                      <ScoreDot score={p.scores.get(d)} />
                      <ProfileMenu day={d} profiles={p.profiles} override={p.overrides.get(d)} onSet={(x) => p.onSetProfile(d, x)} />
                    </span>
                  </div>
                  <ProfileBadge override={p.overrides.get(d)} />
                </div>
              );
            })}
          </div>

          {/* All-day row */}
          <div className="grid border-b border-line" style={gridCols}>
            <div className="px-1 py-1.5 text-right text-[10.5px] text-text-3">all day</div>
            {days.map((d) => {
              const all = buckets.get(d)?.allDay ?? [];
              return (
                <div
                  key={d}
                  className="max-h-28 min-h-9 cursor-cell space-y-1 overflow-y-auto border-l border-line p-1"
                  onDoubleClick={() => p.onCreate({ start: dayStartMs(d, tz), end: dayStartMs(addDays(d, 1), tz), allDay: true })}
                >
                  {all.map((e) => <EntryChip key={entryKey(e)} entry={e} tz={tz} onClick={() => p.onOpen(e)} />)}
                </div>
              );
            })}
          </div>

        </div>
        <div className="grid" style={{ ...gridCols, height: 24 * HOUR_PX }}>
          <div className="relative">
            {Array.from({ length: 23 }, (_, h) => (
              <span key={h} className="absolute right-2 -translate-y-1/2 text-[10.5px] tabular text-text-3" style={{ top: (h + 1) * HOUR_PX }}>
                {String(h + 1).padStart(2, "0")}:00
              </span>
            ))}
          </div>
          <div ref={cols} className="relative col-span-full col-start-2 grid" style={{ gridTemplateColumns: `repeat(${days.length}, minmax(0, 1fr))` }}>
            {/* hour lines */}
            <div aria-hidden className="pointer-events-none absolute inset-0">
              {Array.from({ length: 24 }, (_, h) => (
                <div key={h} className="absolute inset-x-0 border-t border-line/70" style={{ top: h * HOUR_PX }}>
                  <div className="absolute inset-x-0 border-t border-dashed border-line/30" style={{ top: HOUR_PX / 2 }} />
                </div>
              ))}
            </div>
            {days.map((d, i) => {
              const placed = layoutColumn(
                (buckets.get(d)?.timed ?? []).map((t) => ({ item: t.entry, start: t.start, end: t.end })),
                22,
              );
              const off = !p.workdays.includes(weekdayCode(d));
              return (
                <div
                  key={d}
                  data-day={d}
                  className={cn("relative touch-none border-l border-line", off && "bg-white/[.012]")}
                  onPointerDown={(ev) => onColumnDown(ev, i)}
                  onPointerMove={onColumnMove}
                  onPointerUp={onColumnUp}
                  onPointerCancel={() => setSelect(null)}
                >
                  {/* before the day starts (late night still counts as yesterday) */}
                  {p.dayStartHour > 0 ? (
                    <div aria-hidden className="pointer-events-none absolute inset-x-0 top-0 bg-[repeating-linear-gradient(135deg,transparent_0_6px,rgb(255_255_255/.025)_6px_7px)]" style={{ height: p.dayStartHour * HOUR_PX }} />
                  ) : null}

                  {placed.map((pl) => {
                    const e = pl.item;
                    const active = drag?.entry === e && drag.moved && canDrag(e);
                    const top = pl.start * PX + (active && drag!.mode === "move" ? drag!.dMin * PX : 0);
                    let height = Math.max(20, (pl.end - pl.start) * PX - 2);
                    if (active && drag!.mode === "resize") height = Math.max(SNAP * PX, height + drag!.dMin * PX);
                    const lane = showActivity ? 12 : 4;
                    const width = `calc((100% - ${lane}px) * ${pl.span / pl.cols} - 3px)`;
                    const left = `calc((100% - ${lane}px) * ${pl.col / pl.cols} + 2px)`;
                    const Icon = KIND_ICON[e.kind];
                    const done = isDone(e);
                    const short = height < 34;
                    return (
                      <div
                        key={entryKey(e)}
                        role="button"
                        tabIndex={0}
                        onKeyDown={(ev) => onKey(ev, e)}
                        onPointerDown={(ev) => onEntryDown(ev, e, "move", i)}
                        onPointerMove={onEntryMove}
                        onPointerUp={onEntryUp}
                        onPointerCancel={() => setDrag(null)}
                        style={{
                          ...areaVars(e.area),
                          top,
                          height,
                          left,
                          width,
                          transform: active && drag!.dDay ? `translateX(calc(${drag!.dDay * 100}% * ${pl.cols / pl.span} + ${drag!.dDay * 3}px))` : undefined,
                        }}
                        className={cn(
                          "group/e absolute z-[1] touch-none select-none overflow-hidden rounded-md border-l-[3px] border-l-[var(--a)] px-1.5 py-1 text-[12px] leading-4",
                          "bg-[color-mix(in_oklab,var(--a)_16%,black)] ring-1 ring-[color-mix(in_oklab,var(--a)_28%,transparent)] transition-[box-shadow,background] hover:bg-[color-mix(in_oklab,var(--a)_24%,black)]",
                          canDrag(e) ? "cursor-grab" : "cursor-pointer",
                          active && "z-20 cursor-grabbing opacity-90 shadow-[0_10px_30px_-8px_var(--a)] ring-2 ring-[var(--a)]",
                          isPoint(e) && "rounded-full border-l-0 py-0.5 pl-2",
                          e.status === "missed" && "ring-alert/80",
                          done && "opacity-60",
                        )}
                      >
                        <div className={cn("flex min-w-0 items-center gap-1", short ? "" : "mb-0.5")}>
                          <Icon className="size-3 shrink-0 text-[var(--a)]" aria-hidden />
                          <span className={cn("truncate font-medium text-text", done && "line-through decoration-text-3")}>{e.title}</span>
                          {isRecurring(e) ? <Repeat className="size-3 shrink-0 text-text-3" aria-hidden /> : null}
                        </div>
                        <div className={cn("truncate tabular text-[11px] text-text-2", short && "sr-only")}>
                          {active ? previewTime(e, drag!, tz) : timeRange(e.starts_at, e.ends_at, tz)}
                        </div>
                        {canDrag(e) && !isPoint(e) ? (
                          <div
                            aria-hidden
                            onPointerDown={(ev) => onEntryDown(ev, e, "resize", i)}
                            className="absolute inset-x-0 bottom-0 h-2 cursor-ns-resize opacity-0 group-hover/e:opacity-100"
                          >
                            <div className="mx-auto mt-[3px] h-[3px] w-6 rounded-full bg-[var(--a)]/70" />
                          </div>
                        ) : null}
                      </div>
                    );
                  })}

                  {/* actual activity lane */}
                  {showActivity
                    ? (activity.get(d) ?? []).map(({ d: det, start, end }) => (
                        <div
                          key={det.id + start}
                          title={`${det.summary} · ${timeRange(det.started_at, det.ended_at, tz)}`}
                          className="pointer-events-auto absolute right-[2px] z-[2] w-[7px] rounded-full"
                          style={{
                            top: start * PX,
                            height: Math.max(3, (end - start) * PX),
                            background: areaMeta(det.area).color,
                            opacity: det.status === "confirmed" ? 0.95 : 0.55,
                          }}
                          onPointerDown={(ev) => ev.stopPropagation()}
                        />
                      ))
                    : null}

                  {/* selection while dragging to create */}
                  {select && select.day === i ? (
                    <div
                      className="pointer-events-none absolute inset-x-1 z-10 rounded-md border border-accent/70 bg-accent/15 shadow-[0_0_20px_-6px_rgb(34_211_238/.8)]"
                      style={{ top: Math.min(select.a, select.b) * PX, height: Math.max(SNAP, Math.abs(select.b - select.a)) * PX }}
                    >
                      <span className="block px-1.5 py-0.5 text-[11px] tabular text-accent">
                        {minLabel(Math.min(select.a, select.b))}–{minLabel(Math.max(select.a, select.b, Math.min(select.a, select.b) + SNAP))}
                      </span>
                    </div>
                  ) : null}

                  {/* now */}
                  {d === today ? (
                    <div aria-hidden className="pointer-events-none absolute inset-x-0 z-30" style={{ top: nowMin * PX }}>
                      <div className="h-[2px] bg-accent shadow-[0_0_10px_rgb(34_211_238/.9)]" />
                      <div className="absolute -left-[5px] -top-[4px] size-[10px] rounded-full bg-accent shadow-[0_0_10px_rgb(34_211_238/.9)]" />
                    </div>
                  ) : null}
                </div>
              );
            })}
          </div>
        </div>
      </div>
    </div>
  );
}

function minLabel(m: number) {
  const h = Math.floor(m / 60);
  return `${String(h).padStart(2, "0")}:${String(Math.round(m % 60)).padStart(2, "0")}`;
}

function previewTime(e: CalendarEntry, d: Drag, tz: string) {
  const s = Date.parse(e.starts_at);
  const en = Date.parse(e.ends_at);
  if (d.mode === "resize") return timeRange(e.starts_at, new Date(Math.max(s + SNAP * 60_000, en + d.dMin * 60_000)).toISOString(), tz);
  const shift = d.dMin * 60_000;
  return timeRange(new Date(s + shift).toISOString(), new Date(en + shift).toISOString(), tz);
}
