import { useState } from "react";
import { Link } from "react-router";
import { ArrowDownRight, ArrowUpRight, Flame, Gauge, History, MessageSquareWarning, Repeat, Target, Timer } from "lucide-react";
import type { Score } from "@/api/types";
import { readScore, shownScore, useDisputeScore, useScoreHistory, type AreaBreakdown, type Period } from "@/api/scores";
import { ApiError } from "@/api/errors";
import { areaMeta } from "@/lib/areas";
import { adjustmentPoints, band, periodLabel } from "@/lib/scores";
import { fmt, minutesLabel } from "@/lib/time";
import type { Day } from "@/lib/calendar/days";
import { Bar } from "@/ui/Bar";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { Dialog } from "@/ui/Dialog";
import { Field, Textarea } from "@/ui/Field";
import { Ring } from "@/ui/Ring";
import { Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { STATUS_LABEL } from "./ScoreTrend";

const BAND_COLOR = { good: "var(--color-good)", accent: "var(--color-accent)", warn: "var(--color-warn)" } as const;
const SCORED = ["work", "skill", "fitness", "personal"] as const;

export function ScoreDetail({ s, period, tz, onOpenDay }: { s: Score; period: Period; tz: string; onOpenDay: (d: Day) => void }) {
  const { breakdown, consistency, adjustment, coverage } = readScore(s);
  const value = shownScore(s);
  const [dispute, setDispute] = useState(false);
  const [history, setHistory] = useState(false);
  const pct = s.adjustment_pct ?? adjustment?.adjustment_pct ?? 0;
  const pts = adjustmentPoints(pct);
  const reasons = adjustment?.reasons ?? [];
  const status = s.status ?? "final";

  return (
    <div className="grid gap-5 xl:grid-cols-[minmax(0,5fr)_minmax(0,7fr)]">
      <div className="space-y-5">
        <Card className="relative overflow-hidden p-6">
          <div className="pointer-events-none absolute -right-16 -top-16 size-56 rounded-full bg-accent/10 blur-3xl" aria-hidden />
          <p className="text-[12px] font-semibold uppercase tracking-wider text-text-3">{periodLabel(period, s.period_start!, true)}</p>
          <div className="mt-4 flex items-center gap-6">
            <Ring value={(value ?? 0) / 100} size={136} stroke={11} color={value != null ? BAND_COLOR[band(value)] : "var(--color-text-3)"} label={value != null ? `Score ${Math.round(value)} of 100` : "No score"}>
              <span className="font-display text-4xl font-semibold tabular-nums">{value != null ? Math.round(value) : "–"}</span>
            </Ring>
            <div className="min-w-0 space-y-2 text-sm">
              <Badge tone={status === "final" ? "good" : status === "provisional" ? "accent" : "neutral"}>{STATUS_LABEL[status]}</Badge>
              {s.base != null ? (
                <p className="text-text-2">Base <span className="font-mono text-text">{Math.round(s.base)}</span></p>
              ) : null}
              {pct ? (
                <p className={cn("flex items-center gap-1", pct > 0 ? "text-good" : "text-alert-text")}>
                  {pct > 0 ? <ArrowUpRight className="size-4" /> : <ArrowDownRight className="size-4" />}
                  Adjusted {pct > 0 ? "+" : ""}{pct}% <span className="text-text-3">({pts > 0 ? "+" : ""}{pts} pts)</span>
                </p>
              ) : period === "day" && status === "final" ? (
                <p className="text-text-3">No adjustment</p>
              ) : null}
              {coverage ? <p className="text-text-2">{coverage.scored_days} of {coverage.days} days scored</p> : null}
            </div>
          </div>
          {breakdown.reason ? <p className="mt-4 rounded-lg bg-surface-2 px-3 py-2 text-[13px] text-text-2">{breakdown.reason[0]!.toUpperCase() + breakdown.reason.slice(1)}.</p> : null}
          {reasons.length ? (
            <ul className="mt-5 space-y-2.5" aria-label="Why it was adjusted">
              {reasons.map((r, i) => (
                <li key={i} className="flex gap-2.5 text-[13.5px]">
                  <span className={cn("mt-0.5 grid size-5 shrink-0 place-items-center rounded-md", r.direction === "up" ? "bg-good/15 text-good" : "bg-alert/15 text-alert-text")}>
                    {r.direction === "up" ? <ArrowUpRight className="size-3.5" aria-label="raised" /> : <ArrowDownRight className="size-3.5" aria-label="lowered" />}
                  </span>
                  <span className="text-text-2">
                    {r.text}
                    {r.refs?.length ? (
                      <span className="ml-1.5 inline-flex flex-wrap gap-1 align-middle">
                        {r.refs.map((ref) => <RefChip key={`${ref.type}:${ref.id}`} type={ref.type} id={ref.id} />)}
                      </span>
                    ) : null}
                  </span>
                </li>
              ))}
            </ul>
          ) : null}
          {adjustment?.goal_alignment ? (
            <p className="mt-4 text-[12.5px] text-text-3">Goal alignment: <span className="capitalize text-text-2">{adjustment.goal_alignment}</span></p>
          ) : null}
          <div className="mt-5 flex flex-wrap gap-2 border-t border-line pt-4">
            {period === "day" && (status === "final" || status === "provisional") ? (
              <Button size="sm" icon={<MessageSquareWarning className="size-4" />} onClick={() => setDispute(true)}>Dispute</Button>
            ) : null}
            <Button size="sm" variant="ghost" icon={<History className="size-4" />} onClick={() => setHistory(true)}>Versions</Button>
            <span className="ml-auto self-center text-[11.5px] text-text-3">
              {s.profile ?? breakdown.profile ? <>Profile {s.profile ?? breakdown.profile} · </> : null}formula v{s.formula_version ?? "?"}
            </span>
          </div>
        </Card>

        {consistency.items?.length ? (
          <Card>
            <CardHeader title="Consistency" icon={<Repeat className="size-4" />} subtitle={consistency.score != null ? `${Math.round(consistency.score)} · how reliably habits and daily targets happen` : undefined} />
            <ul className="space-y-3 px-5 pb-5">
              {consistency.items.map((c) => (
                <li key={c.id}>
                  <div className="mb-1 flex items-center justify-between gap-3 text-[13px]">
                    <span className="flex min-w-0 items-center gap-1.5 text-text"><Flame className="size-3.5 shrink-0 text-warn" aria-hidden /><span className="truncate">{c.title}</span></span>
                    <span className="shrink-0 font-mono text-[12px] text-text-2">{c.completed}/{c.applicable}</span>
                  </div>
                  <Bar value={c.rate} color="var(--color-warn)" label={`${c.title}: ${Math.round(c.rate * 100)}%`} />
                </li>
              ))}
            </ul>
          </Card>
        ) : null}

        {breakdown.preemptions?.length ? (
          <Card>
            <CardHeader title="What got displaced" icon={<Timer className="size-4" />} subtitle="Entertainment over planned time" />
            <ul className="space-y-2 px-5 pb-5 text-[13px]">
              {breakdown.preemptions.map((p, i) => (
                <li key={i} className="flex items-center gap-2 text-text-2">
                  <span className="size-1.5 shrink-0 rounded-full bg-alert-text" />
                  <span className="min-w-0 flex-1 truncate"><span className="text-text">{p.title}</span> {p.kind === "overlap" ? "overlapped" : "while due"}</span>
                  <span className="font-mono text-[12px]">{minutesLabel(p.minutes)}</span>
                </li>
              ))}
            </ul>
          </Card>
        ) : null}
      </div>

      <div className="space-y-5">
        {breakdown.areas ? <AreaBreakdowns areas={breakdown.areas} weights={breakdown.weights ?? {}} minutes={breakdown.evidence_minutes ?? {}} /> : null}
        {breakdown.days?.length ? <PeriodDays days={breakdown.days} onOpenDay={onOpenDay} dailyMean={breakdown.daily_mean} /> : null}
        {breakdown.weekly_targets?.length ? (
          <Card>
            <CardHeader title="Weekly targets" icon={<Target className="size-4" />} subtitle="20 % of the week score" />
            <ul className="space-y-3 px-5 pb-5">
              {breakdown.weekly_targets.map((t) => (
                <li key={t.id}>
                  <div className="mb-1 flex justify-between text-[13px]"><span className="text-text">{t.title}</span><span className="font-mono text-[12px] text-text-2">{Math.round(t.achieved)}/{t.amount} {t.metric}</span></div>
                  <Bar value={t.ratio} color="var(--color-accent)" label={`${t.title}: ${Math.round(t.ratio * 100)}%`} />
                </li>
              ))}
            </ul>
          </Card>
        ) : null}
      </div>

      <DisputeDialog open={dispute} onOpenChange={setDispute} s={s} />
      <VersionsDialog open={history} onOpenChange={setHistory} s={s} tz={tz} />
    </div>
  );
}

function RefChip({ type, id }: { type: string; id: string }) {
  const label = type.replace(/_/g, " ");
  if (type === "goal" || type === "plan" || type === "target") {
    return <Link to={`/plan?${type}=${id}`} className="rounded-md border border-line px-1.5 text-[11px] capitalize text-accent hover:border-accent/50">{label}</Link>;
  }
  return <span className="rounded-md border border-line px-1.5 text-[11px] capitalize text-text-3">{label}</span>;
}

function AreaBreakdowns({ areas, weights, minutes }: { areas: Record<string, AreaBreakdown>; weights: Record<string, number>; minutes: Record<string, number> }) {
  return (
    <div className="grid gap-4 sm:grid-cols-2">
      {SCORED.map((key) => {
        const a = areas[key];
        const m = areaMeta(key);
        const w = weights[key];
        return (
          <Card key={key} className={cn("p-5", !a?.applicable && "border-dashed bg-transparent")} style={{ "--a": m.color } as React.CSSProperties}>
            <div className="flex items-center gap-2.5">
              <span className="grid size-8 place-items-center rounded-lg bg-[color-mix(in_oklab,var(--a)_16%,transparent)] text-[var(--a)]"><m.icon className="size-4" aria-hidden /></span>
              <div className="min-w-0 flex-1">
                <p className="text-sm font-semibold text-text">{m.label}</p>
                <p className="text-[11.5px] text-text-3">{w != null ? `weight ${Math.round(w * 100)}%` : ""}{minutes[key] ? ` · ${minutesLabel(minutes[key])} seen` : ""}</p>
              </div>
              <span className="font-display text-2xl font-semibold tabular-nums">{a?.score != null ? Math.round(a.score) : "–"}</span>
            </div>
            {!a?.applicable ? (
              <p className="mt-3 text-[12.5px] text-text-3">Nothing planned and no targets: not part of the score.</p>
            ) : (
              <div className="mt-4 space-y-3">
                {a.adherence != null ? <Metric label="Plan adherence" v={a.adherence} color={m.color} /> : null}
                {a.quantity != null ? <Metric label="Targets" v={a.quantity} color={m.color} /> : null}
                {a.bonus ? <p className="text-[12px] text-good">+{a.bonus} bonus for going over target</p> : null}
                {a.items.length ? (
                  <ul className="space-y-1 border-t border-line pt-3">
                    {a.items.map((it) => (
                      <li key={`${it.type}:${it.id}`} className="flex items-center gap-2 text-[12.5px]">
                        <span className={cn("min-w-0 flex-1 truncate", it.excluded ? "text-text-3 line-through" : "text-text-2")} title={it.excluded ? "Excluded" : undefined}>{it.title}</span>
                        <span className="w-8 text-right font-mono text-[11px] text-text-3" title="Weight">×{it.weight}</span>
                        <span className={cn("w-10 text-right font-mono text-[11.5px]", it.completion >= 1 ? "text-good" : it.completion > 0 ? "text-text" : "text-text-3")}>{Math.round(it.completion * 100)}%</span>
                      </li>
                    ))}
                  </ul>
                ) : null}
                {a.targets.length ? (
                  <ul className="space-y-1 border-t border-line pt-3">
                    {a.targets.map((t) => (
                      <li key={t.id} className="flex items-center gap-2 text-[12.5px]">
                        <Target className="size-3 shrink-0 text-text-3" aria-hidden />
                        <span className="min-w-0 flex-1 truncate text-text-2">{t.title}</span>
                        <span className={cn("font-mono text-[11.5px]", t.ratio >= 1 ? "text-good" : "text-text")}>{Math.round(t.achieved)}/{t.amount} {t.metric}</span>
                      </li>
                    ))}
                  </ul>
                ) : null}
              </div>
            )}
          </Card>
        );
      })}
    </div>
  );
}

function Metric({ label, v, color }: { label: string; v: number; color: string }) {
  return (
    <div>
      <div className="mb-1 flex justify-between text-[12.5px]"><span className="text-text-2">{label}</span><span className="font-mono text-text">{Math.round(v * 100)}%</span></div>
      <Bar value={v} color={color} label={`${label} ${Math.round(v * 100)}%`} />
    </div>
  );
}

function PeriodDays({ days, onOpenDay, dailyMean }: { days: { day: Day; status: string; score?: number }[]; onOpenDay: (d: Day) => void; dailyMean?: number }) {
  return (
    <Card>
      <CardHeader title="Days" icon={<Gauge className="size-4" />} subtitle={dailyMean != null ? `Daily mean ${Math.round(dailyMean)}` : "Each scored day counts equally; leave and sick days are left out"} />
      <div className="flex h-36 items-end gap-1 px-5 pb-2">
        {days.map((d) => {
          const v = d.score;
          return (
            <button
              key={d.day}
              onClick={() => onOpenDay(d.day)}
              className="group flex h-full min-w-0 flex-1 flex-col justify-end"
              aria-label={`${periodLabel("day", d.day, true)}: ${v != null ? Math.round(v) : STATUS_LABEL[d.status] ?? d.status}`}
              title={`${periodLabel("day", d.day)}: ${v != null ? Math.round(v) : STATUS_LABEL[d.status] ?? d.status}`}
            >
              {v != null ? (
                <span className="block rounded-t-[4px] transition-[filter] group-hover:brightness-125" style={{ height: `${Math.max(3, v)}%`, background: BAND_COLOR[band(v)] }} />
              ) : (
                <span className="block h-[3px] rounded-full bg-text-3/50" />
              )}
            </button>
          );
        })}
      </div>
      <div className="flex gap-1 px-5 pb-4 text-center text-[10.5px] text-text-3">
        {days.map((d) => <span key={d.day} className="min-w-0 flex-1 truncate">{days.length > 10 ? d.day.slice(8) : periodLabel("day", d.day).slice(0, 3)}</span>)}
      </div>
    </Card>
  );
}

function DisputeDialog({ open, onOpenChange, s }: { open: boolean; onOpenChange: (o: boolean) => void; s: Score }) {
  const [reason, setReason] = useState("");
  const m = useDisputeScore();
  return (
    <Dialog
      open={open}
      onOpenChange={onOpenChange}
      title="Dispute this score"
      description="The assistant re-runs the adjustment once with your reason. Disputes also teach it what it gets wrong."
      footer={
        <>
          <Button variant="ghost" onClick={() => onOpenChange(false)}>Cancel</Button>
          <Button
            variant="primary"
            disabled={!reason.trim()}
            loading={m.isPending}
            onClick={() =>
              m.mutate({ id: s.id, reason: reason.trim() }, {
                onSuccess: () => { toast("Dispute sent", { body: "The score updates when the assistant has looked again." }); onOpenChange(false); setReason(""); },
                onError: (e) => toast.error(e instanceof ApiError ? e.detail ?? e.title : "Couldn't send the dispute"),
              })
            }
          >
            Send dispute
          </Button>
        </>
      }
    >
      <Field label="What does it miss?">
        {(f) => <Textarea {...f} rows={3} autoFocus value={reason} onChange={(e) => setReason(e.target.value)} placeholder="The afternoon went to the release, which is my top goal this month" />}
      </Field>
    </Dialog>
  );
}

function VersionsDialog({ open, onOpenChange, s, tz }: { open: boolean; onOpenChange: (o: boolean) => void; s: Score; tz: string }) {
  const h = useScoreHistory(open ? s.id : null);
  const items = h.data?.items ?? [];
  return (
    <Dialog open={open} onOpenChange={onOpenChange} title="Score versions" description="A score is recomputed when late data or a dispute changes it. Newest first.">
      {h.isPending ? <Skeleton className="h-24" /> : (
        <ol className="max-h-[50vh] space-y-2 overflow-y-auto">
          {items.map((v, i) => (
            <li key={`${v.id}-${v.version}-${i}`} className="flex items-center gap-3 rounded-xl border border-line px-3 py-2 text-[13px]">
              <span className="font-mono text-[11.5px] text-text-3">v{v.version}</span>
              <span className="min-w-0 flex-1 text-text-2">{fmt(v.updated_at, tz, "d MMM, HH:mm")}</span>
              <span className="text-text-3">base <span className="font-mono text-text-2">{v.base != null ? Math.round(v.base) : "–"}</span></span>
              {v.adjustment_pct ? <span className={cn("font-mono", v.adjustment_pct > 0 ? "text-good" : "text-alert-text")}>{v.adjustment_pct > 0 ? "+" : ""}{v.adjustment_pct}%</span> : null}
              <span className="w-8 text-right font-mono font-semibold text-text">{v.final != null ? Math.round(v.final) : "–"}</span>
            </li>
          ))}
          {!items.length ? <p className="text-[13px] text-text-3">Only the current version exists.</p> : null}
        </ol>
      )}
    </Dialog>
  );
}
