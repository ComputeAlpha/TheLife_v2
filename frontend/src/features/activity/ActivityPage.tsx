import { useEffect, useMemo, useState } from "react";
import { Navigate, useNavigate, useParams } from "react-router";
import { CheckCheck, ChevronLeft, ChevronRight, ListChecks, Plus, Radar, SlidersHorizontal, X } from "lucide-react";
import { useUser } from "@/api/auth";
import { useCalendar } from "@/api/calendar";
import { useAddDetection, useConfirmDetections, useDetectionsIn } from "@/api/review";
import { AREAS, type Area, type Detection, type DetectionSource } from "@/api/types";
import {
  applyFilters, areaMinutes, confirmable, NO_FILTERS, visibleHours, type ReviewFilters, type ReviewStatus,
} from "@/lib/activity/review";
import { areaMeta } from "@/lib/areas";
import { addDays, dayStartMs, fmtDay, isValidDay, minutesInto, type Day } from "@/lib/calendar/days";
import { fromLocalInput, localDay, minutesLabel, toLocalInput } from "@/lib/time";
import { useMediaQuery } from "@/lib/useMediaQuery";
import { useNow } from "@/lib/useNow";
import { Button, IconButton } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Sheet } from "@/ui/Dialog";
import { Chip, Segmented } from "@/ui/Segmented";
import { EmptyState, ErrorState, OrbitArt, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { isTyping } from "@/shell/CommandPalette";
import { FormSheet } from "../common/FormSheet";
import { Page } from "../common/Page";
import { ActivitiesLog } from "./ActivitiesLog";
import { DetectionPanel } from "./DetectionPanel";
import { SOURCE_META, STATUS_META } from "./meta";
import { Timeline } from "./Timeline";

/** /activity → today; /activity/:date. */
export function ActivityRoute() {
  const { date } = useParams();
  const user = useUser();
  const now = useNow();
  const today = localDay(now, user.timezone, user.day_start_hour);
  if (!date || !isValidDay(date)) return <Navigate to={`/activity/${today}`} replace />;
  return <ActivityPage day={date} today={today} />;
}

type Tab = "detections" | "activities";
// Adjacent-pair order for the stacked bar that keeps colour-blind separation highest (validated, see §11).
const BAR_ORDER: Area[] = ["other", "work", "skill", "fitness", "entertainment", "personal"];

function ActivityPage({ day, today }: { day: Day; today: Day }) {
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const navigate = useNavigate();
  const [tab, setTab] = useState<Tab>("detections");
  const [filters, setFilters] = useState<ReviewFilters>(NO_FILTERS);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [selecting, setSelecting] = useState(false);
  const [picked, setPicked] = useState<Set<string>>(new Set());
  const [logging, setLogging] = useState(false);
  const [adding, setAdding] = useState(false);
  const wide = useMediaQuery("(min-width: 1024px)");

  const fromMs = dayStartMs(day, tz);
  const toMs = dayStartMs(addDays(day, 1), tz);
  const fromIso = new Date(fromMs).toISOString();
  const toIso = new Date(toMs).toISOString();
  const dets = useDetectionsIn(fromIso, toIso);
  const plan = useCalendar(day, addDays(day, 1), tz);
  const confirm = useConfirmDetections();

  const all = useMemo(() => dets.data ?? [], [dets.data]);
  const shown = useMemo(() => applyFilters(all, filters), [all, filters]);
  const totals = useMemo(() => areaMinutes(all, fromMs, toMs), [all, fromMs, toMs]);
  const toReview = all.filter((d) => (d.status ?? "detected") === "detected").length;
  const selected = selectedId ? all.find((d) => d.id === selectedId) ?? null : null;
  // A correction selects its new detection before the refetch brings it in: hold the panel open meanwhile.
  const arriving = !!selectedId && !selected && dets.isFetching;
  const isToday = day === today;
  const nowMinute = isToday ? minutesInto(now, day, tz) : null;
  const hours = useMemo(
    () =>
      visibleHours(
        [...shown.map((d) => ({ s: d.started_at, e: d.ended_at })), ...plan.items.filter((e) => !e.all_day && e.starts_at && e.ends_at).map((e) => ({ s: e.starts_at!, e: e.ends_at! }))].map((x) => ({
          start: minutesInto(Date.parse(x.s), day, tz),
          end: minutesInto(Date.parse(x.e), day, tz),
        })),
        user.day_start_hour,
        nowMinute,
      ),
    [shown, plan.items, day, tz, user.day_start_hour, nowMinute],
  );

  const go = (d: Day) => {
    setSelectedId(null);
    setPicked(new Set());
    navigate(`/activity/${d}`);
  };

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (isTyping(e) || e.metaKey || e.ctrlKey || e.altKey || document.querySelector("[role=dialog]")) return;
      if (e.key === "ArrowLeft") go(addDays(day, -1));
      else if (e.key === "ArrowRight") go(addDays(day, 1));
      else if (e.key === "t") go(today);
      else if (e.key === "Escape") { setSelectedId(null); setSelecting(false); setPicked(new Set()); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  const bulk = confirmable(all, picked);
  const toggle = <T,>(set: Set<T>, v: T) => {
    const n = new Set(set);
    if (n.has(v)) n.delete(v);
    else n.add(v);
    return n;
  };

  return (
    <Page
      wide
      title="Activity"
      subtitle={isToday ? `Today, ${fmtDay(day, "EEEE d MMMM")}` : fmtDay(day, "EEEE d MMMM yyyy")}
      actions={
        <>
          <IconButton label="Previous day" onClick={() => go(addDays(day, -1))}><ChevronLeft className="size-4" /></IconButton>
          <input
            type="date"
            aria-label="Day"
            value={day}
            max={addDays(today, 1)}
            onChange={(e) => isValidDay(e.target.value) && go(e.target.value)}
            className="h-10 rounded-lg border border-line bg-surface-2 px-3 text-sm text-text [color-scheme:dark]"
          />
          <IconButton label="Next day" onClick={() => go(addDays(day, 1))}><ChevronRight className="size-4" /></IconButton>
          {!isToday ? <Button onClick={() => go(today)}>Today</Button> : null}
          {tab === "activities" ? <Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setLogging(true)}>Log activity</Button> : null}
        </>
      }
    >
      <SummaryBar totals={totals} toReview={toReview} loading={dets.isPending} />

      <div className="mb-4 mt-6 flex flex-wrap items-center gap-3">
        <Segmented<Tab>
          label="Activity views"
          value={tab}
          onChange={setTab}
          options={[
            { value: "detections", label: `Detections${toReview ? ` · ${toReview} to review` : ""}` },
            { value: "activities", label: "Activities" },
          ]}
        />
        {tab === "detections" ? (
          <Button className="ml-auto" icon={<Plus className="size-4" />} onClick={() => setAdding(true)}>Add</Button>
        ) : null}
        {tab === "detections" ? (
          <Button
            variant={selecting ? "primary" : "secondary"}
            icon={selecting ? <X className="size-4" /> : <ListChecks className="size-4" />}
            onClick={() => { setSelecting((s) => !s); setPicked(new Set()); }}
            disabled={!toReview && !selecting}
          >
            {selecting ? "Done selecting" : "Select"}
          </Button>
        ) : null}
      </div>

      {tab === "activities" ? (
        <ActivitiesLog day={day} tz={tz} fromIso={fromIso} toIso={toIso} creating={logging} setCreating={setLogging} />
      ) : (
        <>
          <Filters filters={filters} setFilters={setFilters} toggle={toggle} />
          {selecting ? (
            <div className="sticky top-16 z-20 mb-3 flex flex-wrap items-center gap-3 rounded-xl border border-accent/40 bg-surface-1/95 px-4 py-2.5 shadow-[0_10px_30px_-10px_rgb(34_211_238/.35)] backdrop-blur">
              <span className="text-sm text-text">{bulk.length ? `${bulk.length} selected` : "Tap detections to select them"}</span>
              <Button size="sm" variant="ghost" onClick={() => setPicked(new Set(shown.filter((d) => (d.status ?? "detected") === "detected").map((d) => d.id)))}>
                Select all to review
              </Button>
              <Button
                size="sm"
                variant="primary"
                className="ml-auto"
                icon={<CheckCheck className="size-4" />}
                disabled={!bulk.length}
                loading={confirm.isPending}
                onClick={() =>
                  confirm.mutate(bulk, {
                    onSuccess: (n) => { toast.success(`Confirmed ${n}`); setPicked(new Set()); setSelecting(false); },
                    onError: () => toast.error("Some couldn't be confirmed"),
                  })
                }
              >
                Confirm {bulk.length || ""}
              </Button>
            </div>
          ) : null}
          {dets.isPending ? (
            <Skeleton className="h-[520px]" />
          ) : dets.error ? (
            <ErrorState error={dets.error} retry={() => dets.refetch()} />
          ) : !all.length ? (
            <Card>
              <EmptyState
                art={<OrbitArt className="size-28" />}
                title={day > today ? "This day hasn't happened yet" : "Nothing detected this day"}
                body="When the collector runs on your computer, what you work on shows up here for you to confirm or correct. You can also log activities by hand."
                action={<Button icon={<Plus className="size-4" />} onClick={() => { setTab("activities"); setLogging(true); }}>Log activity</Button>}
              />
            </Card>
          ) : (
            <div className="flex items-start gap-5">
              <div className="min-w-0 flex-1">
                {shown.length ? (
                  <Timeline
                    day={day}
                    tz={tz}
                    hours={hours}
                    detections={shown}
                    planned={plan.items}
                    selectedId={selectedId}
                    onSelect={(d: Detection) => setSelectedId(d.id)}
                    selecting={selecting}
                    picked={picked}
                    onPick={(id) => setPicked((p) => toggle(p, id))}
                    nowMinute={nowMinute}
                  />
                ) : (
                  <Card><EmptyState title="Nothing matches the filters" action={<Button onClick={() => setFilters(NO_FILTERS)}>Clear filters</Button>} /></Card>
                )}
              </div>
              {wide ? (
                <aside className="sticky top-20 hidden max-h-[calc(100dvh-6rem)] w-[380px] shrink-0 overflow-y-auto rounded-2xl border border-line bg-surface-1 p-5 lg:block" aria-label="Detection details">
                  {selected ? (
                    <DetectionPanel key={selected.id} d={selected} tz={tz} onSelectId={setSelectedId} onClose={() => setSelectedId(null)} />
                  ) : arriving ? (
                    <Skeleton className="h-64" />
                  ) : (
                    <EmptyState
                      className="py-10"
                      art={<Radar className="size-9 text-accent" />}
                      title="Pick a block"
                      body="Select a detection to confirm it, dispute it or say what really happened. ← and → change the day."
                    />
                  )}
                </aside>
              ) : (
                <Sheet open={!!selected || arriving} onOpenChange={(o) => !o && setSelectedId(null)} title="Detection" description={selected ? areaMeta(selected.area).label : undefined}>
                  {selected ? <DetectionPanel key={selected.id} d={selected} tz={tz} onSelectId={setSelectedId} /> : <Skeleton className="h-64" />}
                </Sheet>
              )}
            </div>
          )}
        </>
      )}
      {adding ? <AddDetectionSheet day={day} tz={tz} now={now} onClose={() => setAdding(false)} onAdded={(id) => setSelectedId(id)} /> : null}
    </Page>
  );
}

/** Fills a gap the collector missed: what you were doing, when. It counts as yours (source user) and confirmed. */
function AddDetectionSheet({ day, tz, now, onClose, onAdded }: { day: Day; tz: string; now: number; onClose: () => void; onAdded: (id: string) => void }) {
  const add = useAddDetection();
  const end = Math.min(now, dayStartMs(addDays(day, 1), tz) - 60_000);
  return (
    <FormSheet
      id="add-detection-form"
      title="Add what you were doing"
      description="For time the collector didn't see, like a meeting away from your desk. It shows on the timeline as yours."
      submitLabel="Add"
      onClose={onClose}
      initial={{ summary: "", area: "", category: "", started_at: toLocalInput(end - 60 * 60_000, tz), ended_at: toLocalInput(end, tz) }}
      validate={(v): Record<string, string> => {
        const s = fromLocalInput(String(v.started_at), tz), e = fromLocalInput(String(v.ended_at), tz);
        return !s || !e || Date.parse(e) <= Date.parse(s) ? { ended_at: "Ends before it starts" } : {};
      }}
      fields={[
        { key: "summary", label: "What you were doing", type: "text", required: true, maxLength: 500, placeholder: "Design review in the meeting room" },
        { key: "area", label: "Area", type: "area" },
        { key: "category", label: "Category", type: "text", required: true, maxLength: 80, placeholder: "meeting" },
        { key: "started_at", label: "From", type: "datetime" },
        { key: "ended_at", label: "To", type: "datetime" },
      ]}
      onSubmit={async (v) => {
        const r = await add.mutateAsync({ summary: String(v.summary).trim(), area: v.area as Area, category: String(v.category).trim(), started_at: fromLocalInput(String(v.started_at), tz)!, ended_at: fromLocalInput(String(v.ended_at), tz)! });
        toast.success("Added");
        if ("id" in r) onAdded(r.id);
      }}
    />
  );
}

function SummaryBar({ totals, toReview, loading }: { totals: Partial<Record<Area, number>>; toReview: number; loading: boolean }) {
  const parts = BAR_ORDER.filter((a) => (totals[a] ?? 0) > 0).map((a) => ({ area: a, mins: totals[a]! }));
  const sum = parts.reduce((s, p) => s + p.mins, 0);
  if (loading) return <Skeleton className="h-24" />;
  return (
    <Card className="p-5">
      <div className="flex flex-wrap items-baseline gap-x-6 gap-y-1">
        <p className="font-display text-3xl font-semibold tabular-nums text-text">{minutesLabel(sum)}</p>
        <p className="text-sm text-text-2">counted toward your day{toReview ? <> · <span className="text-accent">{toReview} to review</span></> : null}</p>
      </div>
      {sum ? (
        <>
          <div className="mt-4 flex h-3 gap-[2px] overflow-hidden rounded-full" role="img" aria-label={`Counted minutes by area: ${parts.map((p) => `${areaMeta(p.area).label} ${minutesLabel(p.mins)}`).join(", ")}`}>
            {parts.map((p) => (
              <div
                key={p.area}
                title={`${areaMeta(p.area).label}: ${minutesLabel(p.mins)}`}
                className="h-full first:rounded-l-full last:rounded-r-full transition-[flex-grow] duration-500"
                style={{ flexGrow: p.mins, background: areaMeta(p.area).color }}
              />
            ))}
          </div>
          <ul className="mt-3 flex flex-wrap gap-x-5 gap-y-1.5" aria-label="Minutes by area">
            {[...parts].sort((a, b) => b.mins - a.mins).map((p) => {
              const m = areaMeta(p.area);
              return (
                <li key={p.area} className="flex items-center gap-1.5 text-[13px]">
                  <m.icon className="size-3.5" style={{ color: m.color }} aria-hidden />
                  <span className="text-text-2">{m.label}</span>
                  <span className="font-mono text-text">{minutesLabel(p.mins)}</span>
                  <span className="text-text-3">{Math.round((p.mins / sum) * 100)}%</span>
                </li>
              );
            })}
          </ul>
        </>
      ) : (
        <p className="mt-2 text-[13px] text-text-3">Confirmed detections and confident ones (40 % and up) count. Nothing has yet.</p>
      )}
    </Card>
  );
}

function Filters({ filters, setFilters, toggle }: {
  filters: ReviewFilters;
  setFilters: (f: ReviewFilters) => void;
  toggle: <T>(s: Set<T>, v: T) => Set<T>;
}) {
  const count = filters.sources.size + filters.statuses.size + filters.areas.size + (filters.showCorrections ? 1 : 0);
  const active = count > 0;
  const [open, setOpen] = useState(false);
  return (
    <>
    {/* Phones: the chips fold behind one button so the timeline stays near the top. */}
    <Button size="sm" className="mb-3 sm:hidden" icon={<SlidersHorizontal className="size-4" />} aria-expanded={open} onClick={() => setOpen((o) => !o)}>
      Filters{count ? ` · ${count}` : ""}
    </Button>
    <div className={cn("mb-4 flex-wrap items-center gap-x-4 gap-y-2 sm:flex", open ? "flex" : "hidden")} aria-label="Filters" role="group">
      <div className="flex flex-wrap gap-1.5">
        {(Object.keys(SOURCE_META) as DetectionSource[]).map((s) => {
          const m = SOURCE_META[s];
          return (
            <Chip key={s} on={filters.sources.has(s)} onClick={() => setFilters({ ...filters, sources: toggle(filters.sources, s) })}>
              <m.icon className="size-3.5" aria-hidden /> {m.label}
            </Chip>
          );
        })}
      </div>
      <span className="hidden h-5 w-px bg-line sm:block" aria-hidden />
      <div className="flex flex-wrap gap-1.5">
        {(["detected", "confirmed", "disputed"] as ReviewStatus[]).map((s) => (
          <Chip key={s} on={filters.statuses.has(s)} onClick={() => setFilters({ ...filters, statuses: toggle(filters.statuses, s) })}>
            {STATUS_META[s].label}
          </Chip>
        ))}
      </div>
      <span className="hidden h-5 w-px bg-line sm:block" aria-hidden />
      <div className="flex flex-wrap gap-1.5">
        {AREAS.map((a) => {
          const m = areaMeta(a);
          return (
            <Chip key={a} color={m.color} on={filters.areas.has(a)} onClick={() => setFilters({ ...filters, areas: toggle(filters.areas, a) })}>
              <m.icon className="size-3.5" style={{ color: m.color }} aria-hidden /> {m.label}
            </Chip>
          );
        })}
      </div>
      <label className="flex cursor-pointer items-center gap-2 text-[13px] text-text-2">
        <input
          type="checkbox"
          className="size-4 accent-[var(--color-accent)]"
          checked={filters.showCorrections}
          onChange={(e) => setFilters({ ...filters, showCorrections: e.target.checked })}
        />
        Show corrections
      </label>
      {active ? <button className="text-[13px] text-accent hover:underline" onClick={() => setFilters(NO_FILTERS)}>Clear</button> : null}
    </div>
    </>
  );
}
