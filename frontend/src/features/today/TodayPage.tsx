import { useMemo, useState } from "react";
import { Link } from "react-router";
import { motion } from "motion/react";
import {
  AlarmClock, ArrowRight, Bell, Check, CircleSlash, Flame, Laptop, ShieldCheck, Sparkles, ThumbsDown, ThumbsUp, TrendingDown, TrendingUp, X,
} from "lucide-react";
import { useUser } from "@/api/auth";
import { useCalendar, useSetOverride, useTaskAction, useOverrides, BUILTIN_PROFILES, useDayProfiles } from "@/api/calendar";
import {
  useDecideApproval, useDetectionAction, useDevices, usePendingApprovals, usePendingReminders, usePreemptions, useProgressToday,
  useRecentDetections, useRoutines, useScoresRange,
} from "@/api/today";
import { ApiError } from "@/api/errors";
import type { AreaProgress, CalendarEntry, Score } from "@/api/types";
import { AREA_META, areaMeta } from "@/lib/areas";
import { addDays, dayOf, type Day } from "@/lib/calendar/days";
import { entryKey, isPoint } from "@/lib/calendar/entries";
import { fmt, greeting, minutesLabel, relative, timeRange } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Page } from "../common/Page";
import { Bar } from "@/ui/Bar";
import { Card, CardHeader } from "@/ui/Card";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Ring } from "@/ui/Ring";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { KIND_ICON, areaVars, isDone, statusLabel } from "../calendar/EntryChip";
import { entriesOfDay } from "../calendar/MonthView";
import { ProfileMenu, scoreColor } from "../calendar/DayHeader";

export function TodayPage() {
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const today = dayOf(now, tz);
  const first = user.display_name.split(/\s+/)[0];
  const progress = useProgressToday();
  const overrides = useOverrides(today, today);
  const profilesQ = useDayProfiles();
  const setOverride = useSetOverride();
  const override = overrides.data?.items[0];
  const profiles = [...new Set([...BUILTIN_PROFILES, ...(profilesQ.data?.items ?? []).map((p) => p.name)])];

  return (
    <Page
      wide
      title={`${greeting(tz, now)}, ${first}`}
      subtitle={
        <span className="inline-flex flex-wrap items-center gap-2">
          {fmt(now, tz, "EEEE, d MMMM")}
          {progress.data ? (
            <span className="inline-flex items-center gap-1">
              <Badge tone={override ? "accent" : "neutral"} className="capitalize">{progress.data.profile}</Badge>
              <ProfileMenu
                day={today}
                profiles={profiles}
                override={override}
                onSet={(p) =>
                  setOverride.mutate(
                    { day: today, profile: p, existing: override ? { id: override.id, version: override.version } : undefined },
                    { onSuccess: () => toast.success(p ? `Today is a ${p} day` : "Back to the usual day") },
                  )
                }
              />
            </span>
          ) : null}
        </span>
      }
      actions={
        <Link to={`/calendar/day/${today}`} className="inline-flex h-10 items-center gap-2 rounded-[var(--radius-control)] border border-line px-4 text-sm text-text-2 hover:border-line-strong hover:text-text">
          Open calendar <ArrowRight className="size-4" />
        </Link>
      }
    >
      <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
        <DayRingCard progress={progress.data?.areas as Record<string, AreaProgress> | undefined} loading={progress.isPending} />
        <YesterdayCard today={today} />
        <ConsistencyCard today={today} />
        <CollectorCard now={now} />
      </div>

      <div className="mt-4 grid gap-4 xl:grid-cols-3">
        <AgendaCard today={today} tz={tz} now={now} className="xl:col-span-2" />
        <NeedsYouCard tz={tz} now={now} today={today} />
      </div>

      <div className="mt-4 grid gap-4 xl:grid-cols-3">
        <AreasCard progress={progress.data?.areas as Record<string, AreaProgress> | undefined} loading={progress.isPending} error={progress.error} className="xl:col-span-2" />
        <ComingUpCard tz={tz} now={now} />
      </div>
    </Page>
  );
}

// ------------------------------------------------------------------ summary row

function ratio(p: Record<string, AreaProgress> | undefined) {
  if (!p) return null;
  let items = 0, done = 0, mins = 0, covered = 0;
  for (const a of Object.values(p)) {
    items += a.planned_items;
    done += a.done_items;
    mins += a.block_minutes_so_far;
    covered += a.block_minutes_covered;
  }
  const parts: number[] = [];
  if (items) parts.push(done / items);
  if (mins) parts.push(covered / mins);
  return { value: parts.length ? parts.reduce((a, b) => a + b, 0) / parts.length : null, items, done, mins, covered };
}

function StatCard({ title, children, icon, footer }: { title: string; children: React.ReactNode; icon: React.ReactNode; footer?: React.ReactNode }) {
  return (
    <Card className="flex flex-col p-5">
      <div className="mb-3 flex items-center gap-2 text-[13px] font-medium text-text-2">
        <span className="text-accent">{icon}</span>
        {title}
      </div>
      <div className="flex-1">{children}</div>
      {footer ? <div className="mt-3 text-[12.5px] text-text-3">{footer}</div> : null}
    </Card>
  );
}

function DayRingCard({ progress, loading }: { progress?: Record<string, AreaProgress>; loading: boolean }) {
  const r = ratio(progress);
  return (
    <StatCard title="Plan so far" icon={<Sparkles className="size-4" />}>
      {loading ? (
        <Skeleton className="size-24 rounded-full" />
      ) : (
        <div className="flex items-center gap-5">
          <Ring value={r?.value ?? 0} size={96} label={`Plan adherence so far: ${r?.value == null ? "nothing planned yet" : Math.round(r.value * 100) + "%"}`}>
            <span className="font-display text-2xl tabular">{r?.value == null ? "–" : `${Math.round(r.value * 100)}`}<span className="text-sm text-text-3">{r?.value == null ? "" : "%"}</span></span>
          </Ring>
          <dl className="space-y-1.5 text-[13px]">
            <div><dt className="inline text-text-3">Items </dt><dd className="inline tabular text-text">{r?.done ?? 0}/{r?.items ?? 0}</dd></div>
            <div><dt className="inline text-text-3">Blocks </dt><dd className="inline tabular text-text">{minutesLabel(r?.covered ?? 0)} of {minutesLabel(r?.mins ?? 0)}</dd></div>
          </dl>
        </div>
      )}
    </StatCard>
  );
}

function latestFinal(items: Score[] | undefined) {
  return [...(items ?? [])].sort((a, b) => (b.period_start ?? "").localeCompare(a.period_start ?? ""))[0];
}

function YesterdayCard({ today }: { today: Day }) {
  const y = addDays(today, -1);
  const q = useScoresRange("day", y, y);
  const s = latestFinal(q.data?.items);
  const adj = s?.adjustment_pct ?? 0;
  return (
    <StatCard
      title="Yesterday"
      icon={<TrendingUp className="size-4" />}
      footer={<Link to="/scores/day" className="hover:text-accent">How scores work →</Link>}
    >
      {q.isPending ? (
        <Skeleton className="h-16 w-40" />
      ) : !s ? (
        <p className="text-sm text-text-2">No score yet. Scores are made when the day closes.</p>
      ) : s.status === "unobserved" || s.status === "not_scored" ? (
        <p className="text-sm text-text-2">{s.status === "not_scored" ? `Not scored (${s.profile ?? "day off"}).` : "No activity was observed."}</p>
      ) : (
        <div className="flex items-end gap-4">
          <span className="font-display text-5xl leading-none tabular" style={{ color: scoreColor(s.final) }}>{Math.round(s.final ?? 0)}</span>
          <div className="pb-1 text-[13px]">
            {adj ? (
              <p className={cn("flex items-center gap-1", adj > 0 ? "text-good" : "text-alert-text")}>
                {adj > 0 ? <TrendingUp className="size-3.5" /> : <TrendingDown className="size-3.5" />}
                {adj > 0 ? "+" : ""}{Math.round(adj)}% from the assistant
              </p>
            ) : null}
            <p className="text-text-3">base {Math.round(s.base ?? 0)}{s.status === "provisional" ? " · provisional" : ""}</p>
          </div>
        </div>
      )}
    </StatCard>
  );
}

function ConsistencyCard({ today }: { today: Day }) {
  const q = useScoresRange("day", addDays(today, -7), addDays(today, -1));
  const s = latestFinal(q.data?.items.filter((x) => x.consistency && (x.consistency as { items?: unknown[] }).items?.length));
  const items = ((s?.consistency as { items?: { id: string; title: string; completed: number; applicable: number; rate: number }[] })?.items ?? []).slice(0, 3);
  return (
    <StatCard title="Consistency" icon={<Flame className="size-4" />}>
      {q.isPending ? (
        <Skeleton className="h-16" />
      ) : !items.length ? (
        <p className="text-sm text-text-2">Mark a recurring task or target as “track consistency” to see it here.</p>
      ) : (
        <ul className="space-y-2.5">
          {items.map((c) => (
            <li key={c.id}>
              <div className="mb-1 flex justify-between gap-2 text-[13px]">
                <span className="truncate text-text">{c.title}</span>
                <span className="tabular text-text-2">{c.completed}/{c.applicable}</span>
              </div>
              <Bar value={c.rate} color="var(--color-warn)" />
            </li>
          ))}
        </ul>
      )}
    </StatCard>
  );
}

function CollectorCard({ now }: { now: number }) {
  const q = useDevices();
  const devices = (q.data?.items ?? []).filter((d) => !d.revoked_at);
  return (
    <StatCard title="Collector" icon={<Laptop className="size-4" />} footer={<Link to="/settings" className="hover:text-accent">Devices →</Link>}>
      {q.isPending ? (
        <Skeleton className="h-12" />
      ) : !devices.length ? (
        <p className="text-sm text-text-2">No device yet. Register one in Settings to detect activity automatically.</p>
      ) : (
        <ul className="space-y-2">
          {devices.slice(0, 3).map((d) => {
            const seen = d.last_seen_at ? Date.parse(d.last_seen_at) : 0;
            const live = seen && now - seen < 3 * 60_000;
            return (
              <li key={d.id} className="flex items-center gap-2.5 text-[13px]">
                <span className={cn("size-2.5 rounded-full", live ? "bg-good shadow-[0_0_8px_var(--color-good)]" : "bg-text-3")} aria-hidden />
                <span className="truncate text-text">{d.name}</span>
                <span className="ml-auto shrink-0 text-text-3">{live ? "watching" : seen ? relative(seen, now) : "never seen"}</span>
              </li>
            );
          })}
        </ul>
      )}
    </StatCard>
  );
}

// ------------------------------------------------------------------ agenda

function AgendaCard({ today, tz, now, className }: { today: Day; tz: string; now: number; className?: string }) {
  const cal = useCalendar(today, addDays(today, 1), tz);
  const items = useMemo(() => entriesOfDay(cal.items, today, tz), [cal.items, today, tz]);
  const allDay = items.filter((e) => e.all_day);
  const timed = items.filter((e) => !e.all_day);
  const nextIdx = timed.findIndex((e) => Date.parse(e.ends_at) > now || (isPoint(e) && Date.parse(e.starts_at) > now));
  return (
    <Card className={className}>
      <CardHeader
        title="Today's plan"
        subtitle={items.length ? `${items.length} planned · ${items.filter(isDone).length} done` : undefined}
        action={<Link to={`/calendar/day/${today}`} className="text-[13px] text-text-2 hover:text-accent">Day view →</Link>}
      />
      <div className="px-5 pb-5">
        {cal.isPending ? (
          <div className="space-y-2">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-12" />)}</div>
        ) : cal.error ? (
          <ErrorState error={cal.error} retry={cal.refetch} />
        ) : !items.length ? (
          <EmptyState title="A blank day" body="Nothing is planned. Ask the assistant to plan your day, or add something to the calendar." action={<Link to={`/calendar/day/${today}`} className="text-sm text-accent">Plan in the calendar</Link>} />
        ) : (
          <div className="space-y-4">
            {allDay.length ? (
              <ul className="space-y-1.5">
                {allDay.map((e) => <li key={entryKey(e)}><AgendaRow e={e} tz={tz} /></li>)}
              </ul>
            ) : null}
            {timed.length ? (
              <ol className="relative space-y-1.5 border-l border-line pl-4">
                {timed.map((e, i) => (
                  <li key={entryKey(e)} className="relative">
                    {i === nextIdx ? (
                      <span className="absolute -left-[21px] top-0 flex h-full items-start">
                        <span className="mt-4 size-2.5 rounded-full bg-accent shadow-[0_0_10px_var(--color-accent)]" />
                      </span>
                    ) : null}
                    {i === nextIdx && Date.parse(e.starts_at) > now ? <NowDivider now={now} tz={tz} /> : null}
                    <AgendaRow e={e} tz={tz} highlight={i === nextIdx} />
                  </li>
                ))}
                {nextIdx === -1 ? <li><NowDivider now={now} tz={tz} /></li> : null}
              </ol>
            ) : null}
          </div>
        )}
      </div>
    </Card>
  );
}

function NowDivider({ now, tz }: { now: number; tz: string }) {
  return (
    <div className="my-2 flex items-center gap-2 text-[11px] font-medium text-accent" aria-label="Now">
      <span className="tabular">{fmt(now, tz, "HH:mm")}</span>
      <span className="h-px flex-1 bg-gradient-to-r from-accent/70 to-transparent" />
    </div>
  );
}

function AgendaRow({ e, tz, highlight }: { e: CalendarEntry; tz: string; highlight?: boolean }) {
  const act = useTaskAction();
  const Icon = KIND_ICON[e.kind];
  const done = isDone(e);
  const s = statusLabel(e);
  const canAct = e.kind === "task_occurrence" && e.status !== "done" && e.status !== "skipped";
  const run = (verb: "complete" | "skip") =>
    act.mutate({ entry: e, verb }, {
      onSuccess: () => toast.success(verb === "complete" ? `Done: ${e.title}` : `Skipped: ${e.title}`),
      onError: (err) => toast.error("Couldn't update", err instanceof ApiError ? err.summary : undefined),
    });
  return (
    <motion.div
      layout
      style={areaVars(e.area)}
      className={cn(
        "group flex items-center gap-3 rounded-xl border px-3 py-2.5 transition-colors",
        highlight ? "border-[color-mix(in_oklab,var(--a)_55%,transparent)] bg-[color-mix(in_oklab,var(--a)_7%,transparent)]" : "border-line",
      )}
    >
      {e.kind === "task_occurrence" ? (
        <button
          onClick={() => canAct && run("complete")}
          disabled={!canAct || act.isPending}
          aria-label={done ? `${e.title} is done` : `Mark ${e.title} done`}
          className={cn(
            "grid size-6 shrink-0 place-items-center rounded-md border-2 transition-all",
            done ? "border-good bg-good text-black" : "border-[var(--a)] hover:bg-[color-mix(in_oklab,var(--a)_25%,transparent)]",
          )}
        >
          {done ? <Check className="size-4" strokeWidth={3} /> : null}
        </button>
      ) : (
        <span className="grid size-6 shrink-0 place-items-center rounded-md bg-[color-mix(in_oklab,var(--a)_16%,transparent)]">
          <Icon className="size-3.5 text-[var(--a)]" aria-hidden />
        </span>
      )}
      <div className="min-w-0 flex-1">
        <p className={cn("truncate text-sm text-text", done && "text-text-3 line-through")}>{e.title}</p>
        <p className="text-[12px] text-text-3">
          {e.all_day ? "Today" : isPoint(e) ? `Due ${timeRange(e.starts_at, e.starts_at, tz)}` : timeRange(e.starts_at, e.ends_at, tz)}
          {" · "}{areaMeta(e.area).label}{s ? ` · ${s}` : ""}
        </p>
      </div>
      {canAct ? (
        <Button size="sm" variant="ghost" className="opacity-0 group-hover:opacity-100 focus:opacity-100" icon={<CircleSlash className="size-3.5" />} onClick={() => run("skip")}>
          Skip
        </Button>
      ) : null}
    </motion.div>
  );
}

// ------------------------------------------------------------------ needs you

function NeedsYouCard({ tz, now, today }: { tz: string; now: number; today: Day }) {
  const approvals = usePendingApprovals();
  const [since] = useState(() => new Date(Date.now() - 3 * 3_600_000).toISOString());
  const dets = useRecentDetections(since);
  const pre = usePreemptions(addDays(today, -1), today);
  const decide = useDecideApproval();
  const detAct = useDetectionAction();
  const pending = approvals.data?.items ?? [];
  const review = (dets.data?.items ?? []).filter((d) => d.status === "detected").slice(0, 4);
  const preempt = pre.data?.items ?? [];
  const count = pending.length + review.length;
  return (
    <Card>
      <CardHeader title="Needs you" icon={<ShieldCheck className="size-4" />} subtitle={count ? `${count} to look at` : "All clear"} />
      <div className="space-y-5 px-5 pb-5">
        {approvals.isPending || dets.isPending ? <Skeleton className="h-24" /> : null}
        {pending.length ? (
          <section aria-label="Approvals">
            <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Waiting for approval</p>
            <ul className="space-y-2">
              {pending.slice(0, 3).map((a) => (
                <li key={a.id} className="rounded-xl border border-warn/30 bg-warn/[.04] p-3">
                  <p className="text-sm text-text">{a.summary ?? a.tool}</p>
                  <p className="mt-0.5 text-[12px] text-text-3">{a.tool}{a.expires_at ? ` · expires ${relative(a.expires_at, now)}` : ""}</p>
                  <div className="mt-2 flex gap-2">
                    <Button size="sm" variant="primary" icon={<Check className="size-3.5" />} loading={decide.isPending && decide.variables?.id === a.id && decide.variables.verb === "approve"}
                      onClick={() => decide.mutate({ id: a.id, verb: "approve" }, { onSuccess: () => toast.success("Approved") })}>Approve</Button>
                    <Button size="sm" icon={<X className="size-3.5" />}
                      onClick={() => decide.mutate({ id: a.id, verb: "reject" }, { onSuccess: () => toast("Rejected") })}>Reject</Button>
                  </div>
                </li>
              ))}
            </ul>
            {pending.length > 3 ? <Link to="/approvals" className="mt-2 inline-block text-[13px] text-accent">All {pending.length} approvals →</Link> : null}
          </section>
        ) : null}
        {review.length ? (
          <section aria-label="Recent detections">
            <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Was this right?</p>
            <ul className="space-y-2">
              {review.map((d) => {
                const a = areaMeta(d.area);
                return (
                  <li key={d.id} style={areaVars(d.area)} className="flex items-start gap-3 rounded-xl border border-line p-3">
                    <a.icon className="mt-0.5 size-4 shrink-0 text-[var(--a)]" aria-hidden />
                    <div className="min-w-0 flex-1">
                      <p className="line-clamp-2 text-[13px] text-text">{d.summary}</p>
                      <p className="text-[12px] text-text-3">{timeRange(d.started_at, d.ended_at, tz)} · {a.label}</p>
                    </div>
                    <div className="flex shrink-0 gap-1">
                      <button aria-label="Yes, that's right" title="Confirm" className="grid size-8 place-items-center rounded-lg text-text-3 hover:bg-good/15 hover:text-good"
                        onClick={() => detAct.mutate({ id: d.id, verb: "confirm" }, { onSuccess: () => toast.success("Confirmed") })}>
                        <ThumbsUp className="size-4" />
                      </button>
                      <button aria-label="No, dispute it" title="Dispute" className="grid size-8 place-items-center rounded-lg text-text-3 hover:bg-alert/15 hover:text-alert-text"
                        onClick={() => detAct.mutate({ id: d.id, verb: "dispute" }, { onSuccess: () => toast("Disputed: the assistant will take another look") })}>
                        <ThumbsDown className="size-4" />
                      </button>
                    </div>
                  </li>
                );
              })}
            </ul>
          </section>
        ) : null}
        {preempt.length ? (
          <section aria-label="What got displaced">
            <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Displaced</p>
            <ul className="space-y-1.5 text-[13px] text-text-2">
              {preempt.slice(0, 3).map((x, i) => (
                <li key={i} className="flex gap-2">
                  <span className="mt-1.5 size-1.5 shrink-0 rounded-full bg-alert-text" />
                  <span>
                    {x.kind === "overlap" ? "Entertainment overlapped" : "Entertainment while"} <span className="text-text">{x.displaced_title ?? x.displaced_type}</span>
                    {" "}for {minutesLabel(x.minutes)}{x.local_day !== today ? " yesterday" : ""}
                  </span>
                </li>
              ))}
            </ul>
          </section>
        ) : null}
        {!approvals.isPending && !dets.isPending && !count && !preempt.length ? (
          <EmptyState className="py-6" title="Nothing waiting" body="Approvals and things to review will show up here." />
        ) : null}
      </div>
    </Card>
  );
}

// ------------------------------------------------------------------ areas and coming up


function AreasCard({ progress, loading, error, className }: { progress?: Record<string, AreaProgress>; loading: boolean; error: unknown; className?: string }) {
  const areas = Object.entries(progress ?? {});
  return (
    <Card className={className}>
      <CardHeader title="By area" subtitle="Planned items, time blocks and targets so far today." />
      <div className="px-5 pb-5">
        {loading ? (
          <Skeleton className="h-32" />
        ) : error ? (
          <ErrorState error={error} />
        ) : !areas.length ? (
          <p className="text-sm text-text-2">Nothing planned or targeted today yet.</p>
        ) : (
          <div className="grid gap-x-8 gap-y-5 sm:grid-cols-2">
            {areas.map(([area, p]) => {
              const m = AREA_META[area as keyof typeof AREA_META] ?? areaMeta(area);
              return (
                <div key={area} style={areaVars(area)}>
                  <div className="mb-2 flex items-center gap-2">
                    <m.icon className="size-4 text-[var(--a)]" aria-hidden />
                    <span className="text-sm font-medium text-text">{m.label}</span>
                  </div>
                  <div className="space-y-2 text-[12.5px]">
                    {p.planned_items ? (
                      <div>
                        <div className="mb-1 flex justify-between text-text-2"><span>Items</span><span className="tabular">{p.done_items}/{p.planned_items}</span></div>
                        <Bar value={p.done_items / p.planned_items} color={m.color} />
                      </div>
                    ) : null}
                    {p.block_minutes_so_far ? (
                      <div>
                        <div className="mb-1 flex justify-between text-text-2"><span>Blocks</span><span className="tabular">{minutesLabel(p.block_minutes_covered)} / {minutesLabel(p.block_minutes_so_far)}</span></div>
                        <Bar value={p.block_minutes_covered / p.block_minutes_so_far} color={m.color} />
                      </div>
                    ) : null}
                    {p.targets.map((t) => (
                      <div key={t.title}>
                        <div className="mb-1 flex justify-between text-text-2">
                          <span className="truncate">{t.title}</span>
                          <span className="shrink-0 tabular">{t.metric === "minutes" ? `${minutesLabel(t.achieved)} / ${minutesLabel(t.amount)}` : `${t.achieved} / ${t.amount} ${t.metric}`}</span>
                        </div>
                        <Bar value={t.amount ? t.achieved / t.amount : 0} color={m.color} />
                      </div>
                    ))}
                  </div>
                </div>
              );
            })}
          </div>
        )}
      </div>
    </Card>
  );
}

function ComingUpCard({ tz, now }: { tz: string; now: number }) {
  const rem = usePendingReminders();
  const rout = useRoutines();
  const reminders = [...(rem.data?.items ?? [])].filter((r) => Date.parse(r.due_at) >= now - 3_600_000).sort((a, b) => a.due_at.localeCompare(b.due_at)).slice(0, 4);
  const routines = (rout.data?.items ?? []).filter((r) => r.enabled).sort((a, b) => (a.learned_time ?? a.default_time).localeCompare(b.learned_time ?? b.default_time));
  return (
    <Card>
      <CardHeader title="Coming up" icon={<Bell className="size-4" />} />
      <div className="space-y-5 px-5 pb-5">
        <section>
          <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Reminders</p>
          {rem.isPending ? <Skeleton className="h-10" /> : !reminders.length ? (
            <p className="text-[13px] text-text-3">None pending. Ask the assistant: “remind me to call mom at 7”.</p>
          ) : (
            <ul className="space-y-2">
              {reminders.map((r) => (
                <li key={r.id} className="flex items-start gap-2.5 text-[13px]">
                  <AlarmClock className="mt-0.5 size-4 shrink-0 text-accent" aria-hidden />
                  <span className="min-w-0 flex-1 text-text">{r.text}</span>
                  <span className="shrink-0 tabular text-text-3">{dayOf(Date.parse(r.due_at), tz) === dayOf(now, tz) ? fmt(r.due_at, tz, "HH:mm") : fmt(r.due_at, tz, "EEE HH:mm")}</span>
                </li>
              ))}
            </ul>
          )}
        </section>
        <section>
          <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Check-ins</p>
          {rout.isPending ? <Skeleton className="h-10" /> : !routines.length ? (
            <p className="text-[13px] text-text-3">No routines on.</p>
          ) : (
            <ul className="space-y-2">
              {routines.map((r) => (
                <li key={r.id} className="flex items-center gap-2.5 text-[13px]">
                  <span className="size-1.5 rounded-full bg-accent" aria-hidden />
                  <span className="flex-1 truncate text-text">{r.title}</span>
                  <span className="tabular text-text-3">{(r.learned_time ?? r.default_time).slice(0, 5)}{r.mode === "adaptive" ? " ~" : ""}</span>
                </li>
              ))}
            </ul>
          )}
        </section>
      </div>
    </Card>
  );
}

