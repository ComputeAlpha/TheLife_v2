import { useEffect, useMemo, useState } from "react";
import { Navigate, useNavigate, useParams } from "react-router";
import { useQueryClient } from "@tanstack/react-query";
import * as P from "@radix-ui/react-popover";
import { Activity, CalendarDays, ChevronLeft, ChevronRight, Filter, Plus, RefreshCw } from "lucide-react";
import { useUser } from "@/api/auth";
import {
  BUILTIN_PROFILES, useCalendar, useDayProfiles, useDayScores, useDetectionsBetween, useOverrides, usePatchEvent,
  usePatchTaskDue, usePatchTimed, useSetOverride,
} from "@/api/calendar";
import { ApiError } from "@/api/errors";
import { AREAS, type Area, type CalendarEntry, type DayProfileOverride, type Score } from "@/api/types";
import {
  addDays, addMonths, dayOf, fmtDay, isValidDay, monthGrid, range, startOfWeek, startOfYear, weekdayCode, type Day,
} from "@/lib/calendar/days";
import { KIND_LABEL, entryKey, windowInstants, type EntryKind } from "@/lib/calendar/entries";
import { AREA_META } from "@/lib/areas";
import { useNow } from "@/lib/useNow";
import { isTyping } from "@/shell/CommandPalette";
import { Button, IconButton } from "@/ui/Button";
import { Kbd } from "@/ui/Badge";
import { ErrorState } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { TimeGrid } from "./TimeGrid";
import { MonthView } from "./MonthView";
import { YearView, type YearMetric } from "./YearView";
import { AgendaView } from "./AgendaView";
import { EntrySheet } from "./EntrySheet";
import { QuickCreate, type CreateDraft } from "./QuickCreate";
import { KIND_ICON } from "./EntryChip";

export const VIEWS = ["day", "workweek", "week", "month", "year", "agenda"] as const;
export type View = (typeof VIEWS)[number];
const VIEW_LABEL: Record<View, string> = { day: "Day", workweek: "Work week", week: "Week", month: "Month", year: "Year", agenda: "Agenda" };
const VIEW_KEY: Record<string, View> = { d: "day", x: "workweek", w: "week", m: "month", y: "year", a: "agenda" };

/** The days a view covers and the window to fetch. */
export function viewRange(view: View, day: Day, workdays: string[]): { days: Day[]; from: Day; to: Day } {
  switch (view) {
    case "day":
      return { days: [day], from: day, to: addDays(day, 1) };
    case "week":
    case "workweek": {
      const mon = startOfWeek(day);
      const all = range(mon, 7);
      const days = view === "week" ? all : all.filter((d) => workdays.includes(weekdayCode(d)));
      return { days: days.length ? days : all, from: mon, to: addDays(mon, 7) };
    }
    case "month": {
      const g = monthGrid(day);
      return { days: g, from: g[0]!, to: addDays(g[41]!, 1) };
    }
    case "year": {
      const y = startOfYear(day);
      return { days: [], from: y, to: addMonths(y, 12) };
    }
    case "agenda":
      return { days: range(day, 30), from: day, to: addDays(day, 30) };
  }
}

export function step(view: View, day: Day, dir: 1 | -1): Day {
  switch (view) {
    case "day": return addDays(day, dir);
    case "week":
    case "workweek": return addDays(day, 7 * dir);
    case "month": return addMonths(day, dir);
    case "year": return addMonths(day, 12 * dir);
    case "agenda": return addDays(day, 30 * dir);
  }
}

function title(view: View, day: Day, days: Day[]): string {
  switch (view) {
    case "day": return fmtDay(day, "EEEE, d MMMM yyyy");
    case "week":
    case "workweek": {
      const a = days[0]!, b = days.at(-1)!;
      return a.slice(0, 7) === b.slice(0, 7) ? `${fmtDay(a, "d")} – ${fmtDay(b, "d MMMM yyyy")}` : `${fmtDay(a, "d MMM")} – ${fmtDay(b, "d MMM yyyy")}`;
    }
    case "month": return fmtDay(day, "MMMM yyyy");
    case "year": return day.slice(0, 4);
    case "agenda": return `Next 30 days from ${fmtDay(day, "d MMM")}`;
  }
}

const KINDS: EntryKind[] = ["event", "task_occurrence", "skill_session", "entertainment_slot"];
const PREFS = "thelife.calendar.prefs";
interface Prefs { kinds: EntryKind[]; areas: Area[]; activity: boolean; yearMetric: YearMetric }
function loadPrefs(): Prefs {
  const d: Prefs = { kinds: KINDS, areas: [...AREAS], activity: false, yearMetric: "planned" };
  try {
    return { ...d, ...JSON.parse(localStorage.getItem(PREFS) ?? "{}") };
  } catch {
    return d;
  }
}

export function CalendarRoute() {
  const { view, date } = useParams();
  const user = useUser();
  const now = useNow();
  const today = dayOf(now, user.timezone);
  if (!view || !VIEWS.includes(view as View) || !isValidDay(date)) {
    const fallback: View = typeof window !== "undefined" && window.innerWidth < 768 ? "day" : "week";
    return <Navigate to={`/calendar/${VIEWS.includes(view as View) ? view : fallback}/${isValidDay(date) ? date : today}`} replace />;
  }
  return <CalendarPage view={view as View} day={date} />;
}

function CalendarPage({ view, day }: { view: View; day: Day }) {
  const user = useUser();
  const tz = user.timezone;
  const workdays = useMemo(() => user.workdays ?? ["MO", "TU", "WE", "TH", "FR"], [user.workdays]);
  const now = useNow(30_000);
  const today = dayOf(now, tz);
  const navigate = useNavigate();
  const qc = useQueryClient();
  const [prefs, setPrefsState] = useState<Prefs>(loadPrefs);
  const setPrefs = (p: Partial<Prefs>) =>
    setPrefsState((cur) => {
      const next = { ...cur, ...p };
      try { localStorage.setItem(PREFS, JSON.stringify(next)); } catch { /* convenience only */ }
      return next;
    });
  const [open, setOpen] = useState<CalendarEntry | null>(null);
  const [draft, setDraft] = useState<CreateDraft | null>(null);
  const [moves, setMoves] = useState<Map<string, { starts_at: string; ends_at: string }>>(new Map());

  const { days, from, to } = useMemo(() => viewRange(view, day, workdays), [view, day, workdays]);
  const cal = useCalendar(from, to, tz);
  const scoresQ = useDayScores(from, addDays(to, -1));
  const overridesQ = useOverrides(from, addDays(to, -1));
  const profilesQ = useDayProfiles();
  const timeView = view === "day" || view === "week" || view === "workweek";
  const inst = windowInstants(from, to, tz);
  const detQ = useDetectionsBetween(inst.from, inst.to, timeView && prefs.activity);
  const setOverride = useSetOverride();
  const patchEvent = usePatchEvent();
  const patchTimed = usePatchTimed();
  const patchDue = usePatchTaskDue();

  const entries = useMemo(
    () =>
      cal.items
        .filter((e) => prefs.kinds.includes(e.kind) && (!e.area || prefs.areas.includes(e.area as Area)))
        .map((e) => {
          const m = moves.get(entryKey(e));
          return m ? { ...e, ...m } : e;
        }),
    [cal.items, prefs.kinds, prefs.areas, moves],
  );
  const scores = useMemo(() => new Map<Day, Score>((scoresQ.data?.items ?? []).map((s) => [s.period_start as Day, s])), [scoresQ.data]);
  const overrides = useMemo(() => new Map<Day, DayProfileOverride>((overridesQ.data?.items ?? []).map((o) => [o.local_day as Day, o])), [overridesQ.data]);
  const profiles = useMemo(() => [...new Set([...BUILTIN_PROFILES, ...(profilesQ.data?.items ?? []).map((p) => p.name)])], [profilesQ.data]);

  const go = (v: View, d: Day) => navigate(`/calendar/${v}/${d}`);
  const goDay = (d: Day) => go("day", d);

  // Keyboard: t today, d/x/w/m/y/a views, ←/→ step, n new.
  useEffect(() => {
    const k = (e: KeyboardEvent) => {
      if (isTyping(e) || e.metaKey || e.ctrlKey || e.altKey || document.querySelector("[role=dialog]")) return;
      if (e.key === "t") go(view, today);
      else if (e.key === "ArrowLeft") go(view, step(view, day, -1));
      else if (e.key === "ArrowRight") go(view, step(view, day, 1));
      else if (e.key === "n") newAt();
      else if (VIEW_KEY[e.key]) go(VIEW_KEY[e.key]!, day);
      else return;
      e.preventDefault();
    };
    window.addEventListener("keydown", k);
    return () => window.removeEventListener("keydown", k);
  });

  function newAt() {
    const base = Date.parse(windowInstants(day === today ? today : day, addDays(day, 1), tz).from);
    const start = day === today ? Math.ceil(now / 1_800_000) * 1_800_000 : base + 9 * 3_600_000;
    setDraft({ start, end: start + 3_600_000 });
  }

  const onMove = (e: CalendarEntry, s: number, en: number) => {
    const key = entryKey(e);
    const patch = { starts_at: new Date(s).toISOString(), ends_at: new Date(en).toISOString() };
    setMoves((m) => new Map(m).set(key, patch));
    const settle = {
      onSuccess: async () => {
        await qc.refetchQueries({ queryKey: ["calendar"] });
        setMoves((m) => { const n = new Map(m); n.delete(key); return n; });
        toast.success("Rescheduled", `${e.title} · ${fmtDay(dayOf(s, tz), "EEE d MMM")}`);
      },
      onError: (err: unknown) => {
        setMoves((m) => { const n = new Map(m); n.delete(key); return n; });
        toast.error("Couldn't move it", err instanceof ApiError ? err.summary : undefined);
      },
    };
    if (e.kind === "event") patchEvent.mutate({ id: e.id, patch }, settle);
    else if (e.kind === "task_occurrence") patchDue.mutate({ id: e.id, due_at: patch.starts_at }, settle);
    else patchTimed.mutate({ kind: e.kind, id: e.id, patch }, settle);
  };

  const onSetProfile = (d: Day, p: string | null) => {
    const ex = overrides.get(d);
    setOverride.mutate(
      { day: d, profile: p, existing: ex ? { id: ex.id, version: ex.version } : undefined },
      {
        onSuccess: () => toast.success(p ? `${fmtDay(d, "EEE d MMM")} is now a ${p} day` : "Back to the usual day"),
        onError: (err) => toast.error("Couldn't change the day", err instanceof ApiError ? err.summary : undefined),
      },
    );
  };

  return (
    <div className="flex h-[calc(100dvh-4rem-4.5rem)] flex-col lg:h-[calc(100dvh-4rem)]">
      {/* Toolbar */}
      <div className="flex flex-wrap items-center gap-2 border-b border-line px-3 py-3 sm:px-5">
        <div className="flex items-center gap-1">
          <Button size="sm" onClick={() => go(view, today)} title="Today (t)">Today</Button>
          <IconButton size="sm" label="Previous (←)" onClick={() => go(view, step(view, day, -1))}><ChevronLeft className="size-4" /></IconButton>
          <IconButton size="sm" label="Next (→)" onClick={() => go(view, step(view, day, 1))}><ChevronRight className="size-4" /></IconButton>
        </div>
        <DatePicker day={day} onPick={(d) => go(view, d)} label={title(view, day, days)} />
        {cal.isFetching ? <RefreshCw className="size-3.5 animate-spin text-text-3" aria-label="Updating" /> : null}

        <div className="ml-auto flex flex-wrap items-center gap-2">
          {timeView ? (
            <Button size="sm" aria-label="Show activity" variant={prefs.activity ? "secondary" : "ghost"} aria-pressed={prefs.activity}
              icon={<Activity className={cn("size-4", prefs.activity && "text-accent")} />}
              onClick={() => setPrefs({ activity: !prefs.activity })} title="Show what was detected next to the plan">
              <span className="hidden sm:inline">Activity</span>
            </Button>
          ) : null}
          {view === "year" ? (
            <div className="flex rounded-lg border border-line p-0.5 text-[12.5px]" role="group" aria-label="Shade days by">
              {(["planned", "score"] as const).map((m) => (
                <button key={m} aria-pressed={prefs.yearMetric === m} onClick={() => setPrefs({ yearMetric: m })}
                  className={cn("rounded-md px-2.5 py-1 capitalize", prefs.yearMetric === m ? "bg-surface-3 text-text" : "text-text-3 hover:text-text")}>
                  {m}
                </button>
              ))}
            </div>
          ) : null}
          <Filters prefs={prefs} setPrefs={setPrefs} />
          <ViewSwitch view={view} onView={(v) => go(v, day)} />
          <Button size="sm" aria-label="New" variant="primary" icon={<Plus className="size-4" />} onClick={newAt} title="New (n)">
            <span className="hidden sm:inline">New</span>
          </Button>
        </div>
      </div>

      {cal.truncated ? (
        <p className="border-b border-warn/30 bg-warn/5 px-5 py-1.5 text-[12.5px] text-warn">Too many entries in this range; some are not shown. Try a shorter view.</p>
      ) : null}

      <div className="min-h-0 flex-1 overflow-auto">
        {cal.error && !cal.items.length ? (
          <ErrorState error={cal.error} retry={cal.refetch} />
        ) : timeView ? (
          <TimeGrid
            days={days} entries={entries} tz={tz} now={now} workdays={workdays} dayStartHour={user.day_start_hour}
            scores={scores} overrides={overrides} profiles={profiles}
            detections={prefs.activity ? (detQ.data?.items ?? []) : null}
            onCreate={setDraft} onOpen={setOpen} onMove={onMove} onDay={goDay} onSetProfile={onSetProfile}
          />
        ) : view === "month" ? (
          <MonthView day={day} entries={entries} tz={tz} now={now} workdays={workdays} scores={scores} overrides={overrides}
            onOpen={setOpen} onDay={goDay} onCreate={setDraft} />
        ) : view === "year" ? (
          <YearView year={startOfYear(day)} entries={entries} scores={scores} tz={tz} now={now} metric={prefs.yearMetric} onDay={goDay} />
        ) : (
          <AgendaView from={day} days={30} entries={entries} tz={tz} now={now} onOpen={setOpen} />
        )}
      </div>

      <EntrySheet entry={open} tz={tz} onClose={() => setOpen(null)} />
      <QuickCreate draft={draft} tz={tz} onClose={() => setDraft(null)} />
    </div>
  );
}

function ViewSwitch({ view, onView }: { view: View; onView: (v: View) => void }) {
  const keys: Record<View, string> = { day: "d", workweek: "x", week: "w", month: "m", year: "y", agenda: "a" };
  return (
    <>
      <div className="hidden rounded-lg border border-line p-0.5 md:flex" role="tablist" aria-label="Calendar view">
        {VIEWS.map((v) => (
          <button key={v} role="tab" aria-selected={view === v} onClick={() => onView(v)} title={`${VIEW_LABEL[v]} (${keys[v]})`}
            className={cn("relative rounded-md px-2.5 py-1 text-[13px] transition-colors",
              view === v ? "bg-accent/12 text-text shadow-[inset_0_0_0_1px_rgb(34_211_238/.4)]" : "text-text-2 hover:text-text")}>
            {VIEW_LABEL[v]}
          </button>
        ))}
      </div>
      <select aria-label="Calendar view" value={view} onChange={(e) => onView(e.target.value as View)}
        className="h-8 rounded-lg border border-line bg-surface-2 px-2 text-[13px] text-text md:hidden">
        {VIEWS.map((v) => <option key={v} value={v}>{VIEW_LABEL[v]}</option>)}
      </select>
    </>
  );
}

function Filters({ prefs, setPrefs }: { prefs: Prefs; setPrefs: (p: Partial<Prefs>) => void }) {
  const active = prefs.kinds.length < KINDS.length || prefs.areas.length < AREAS.length;
  const toggle = <T,>(list: T[], x: T) => (list.includes(x) ? list.filter((y) => y !== x) : [...list, x]);
  return (
    <P.Root>
      <P.Trigger asChild>
        <Button size="sm" aria-label="Filter" variant={active ? "secondary" : "ghost"} icon={<Filter className={cn("size-4", active && "text-accent")} />}>
          <span className="hidden sm:inline">Filter</span>
        </Button>
      </P.Trigger>
      <P.Portal>
        <P.Content align="end" sideOffset={8} className="z-50 w-72 space-y-4 rounded-xl border border-line-strong bg-bg p-4 shadow-2xl">
          <div>
            <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Show</p>
            <div className="space-y-1">
              {KINDS.map((k) => {
                const Icon = KIND_ICON[k];
                return (
                  <label key={k} className="flex cursor-pointer items-center gap-2.5 rounded-lg px-2 py-1.5 text-sm text-text-2 hover:bg-surface-2">
                    <input type="checkbox" className="size-4 accent-[var(--color-accent)]" checked={prefs.kinds.includes(k)} onChange={() => setPrefs({ kinds: toggle(prefs.kinds, k) })} />
                    <Icon className="size-4 text-text-3" /> {KIND_LABEL[k]}s
                  </label>
                );
              })}
            </div>
          </div>
          <div>
            <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Areas</p>
            <div className="flex flex-wrap gap-1.5">
              {AREAS.map((a) => {
                const m = AREA_META[a];
                const on = prefs.areas.includes(a);
                return (
                  <button key={a} aria-pressed={on} onClick={() => setPrefs({ areas: toggle(prefs.areas, a) })}
                    style={{ "--a": m.color } as React.CSSProperties}
                    className={cn("inline-flex h-7 items-center gap-1.5 rounded-full border px-2.5 text-[12px]",
                      on ? "border-[var(--a)] bg-[color-mix(in_oklab,var(--a)_14%,transparent)] text-text" : "border-line text-text-3")}>
                    <m.icon className="size-3 text-[var(--a)]" /> {m.label}
                  </button>
                );
              })}
            </div>
          </div>
          {active ? <Button size="sm" variant="ghost" onClick={() => setPrefs({ kinds: KINDS, areas: [...AREAS] })}>Show everything</Button> : null}
        </P.Content>
      </P.Portal>
    </P.Root>
  );
}

function DatePicker({ day, onPick, label }: { day: Day; onPick: (d: Day) => void; label: string }) {
  const [month, setMonth] = useState(day);
  return (
    <P.Root onOpenChange={(o) => o && setMonth(day)}>
      <P.Trigger className="flex items-center gap-2 rounded-lg px-2 py-1 font-display text-[17px] font-semibold hover:bg-surface-2 sm:text-lg">
        <CalendarDays className="size-4 text-accent" aria-hidden />
        {label}
      </P.Trigger>
      <P.Portal>
        <P.Content align="start" sideOffset={8} className="z-50 w-72 rounded-xl border border-line-strong bg-bg p-3 shadow-2xl">
          <div className="mb-2 flex items-center justify-between">
            <IconButton size="sm" label="Previous month" onClick={() => setMonth(addMonths(month, -1))}><ChevronLeft className="size-4" /></IconButton>
            <span className="text-sm font-medium">{fmtDay(month, "MMMM yyyy")}</span>
            <IconButton size="sm" label="Next month" onClick={() => setMonth(addMonths(month, 1))}><ChevronRight className="size-4" /></IconButton>
          </div>
          <div className="grid grid-cols-7 gap-0.5 text-center">
            {["M", "T", "W", "T", "F", "S", "S"].map((w, i) => <span key={i} className="py-1 text-[11px] text-text-3">{w}</span>)}
            {monthGrid(month).map((d) => (
              <P.Close key={d} asChild>
                <button onClick={() => onPick(d)}
                  className={cn("h-8 rounded-md text-[13px] tabular hover:bg-surface-3",
                    !d.startsWith(month.slice(0, 7)) && "text-text-3",
                    d === day && "bg-accent font-semibold text-accent-ink hover:bg-accent-strong")}>
                  {Number(d.slice(8))}
                </button>
              </P.Close>
            ))}
          </div>
          <p className="mt-3 flex flex-wrap gap-x-3 gap-y-1 text-[11px] text-text-3">
            <span><Kbd>t</Kbd> today</span><span><Kbd>←</Kbd><Kbd>→</Kbd> move</span><span><Kbd>d</Kbd><Kbd>w</Kbd><Kbd>m</Kbd><Kbd>y</Kbd> views</span>
          </p>
        </P.Content>
      </P.Portal>
    </P.Root>
  );
}
