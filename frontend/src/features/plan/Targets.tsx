import { useState } from "react";
import { Flame, Flag, Plus, Target as TargetIcon } from "lucide-react";
import type { AreaProgress, Goal, Target } from "@/api/types";
import { useDeleteTarget, useSaveTarget } from "@/api/plan";
import { BUILTIN_PROFILES, useDayProfiles } from "@/api/calendar";
import { useProgressToday } from "@/api/today";
import { areaMeta } from "@/lib/areas";
import { fmtDay, type Day } from "@/lib/calendar/days";
import { AreaPicker } from "@/ui/AreaPicker";
import { Bar } from "@/ui/Bar";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, NativeSelect } from "@/ui/Field";
import { Chip, Segmented } from "@/ui/Segmented";
import { EmptyState } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { areaVars } from "../calendar/EntryChip";
import { HORIZON_META } from "./Goals";
import { DeleteButton, errText, fieldErr } from "./shared";

const METRICS: Target["metric"][] = ["minutes", "pages", "sections", "sessions", "count"];

function amountText(t: { amount: number; metric: string }) {
  return t.metric === "minutes" && t.amount >= 60 && t.amount % 30 === 0 ? `${t.amount / 60} h` : `${t.amount} ${t.metric}`;
}

function isActive(t: Target, today: Day) {
  return (!t.active_from || t.active_from <= today) && (!t.active_to || t.active_to >= today);
}

export function TargetsView({ targets, goals, today, onEdit, highlight }: { targets: Target[]; goals: Goal[]; today: Day; onEdit: (t: Target | "new") => void; highlight: string | null }) {
  const progress = useProgressToday();
  const areas = (progress.data?.areas ?? {}) as Record<string, AreaProgress>;
  const todayProfile = progress.data?.profile;
  const byGoal = new Map(goals.map((g) => [g.id, g]));
  if (!targets.length) {
    return (
      <Card>
        <EmptyState
          art={<TargetIcon className="size-10 text-accent" />}
          title="No targets yet"
          body="Targets are amounts to hit, like 45 minutes of reading a day or 3 gym sessions a week. They count toward the area's score."
          action={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => onEdit("new")}>Add a target</Button>}
        />
      </Card>
    );
  }
  const sorted = [...targets].sort((a, b) => Number(isActive(b, today)) - Number(isActive(a, today)) || (a.period ?? "day").localeCompare(b.period ?? "day") || a.title.localeCompare(b.title));
  return (
    <div className="grid gap-4 md:grid-cols-2 2xl:grid-cols-3">
      {sorted.map((t) => {
        const a = areaMeta(t.area);
        const active = isActive(t, today);
        const daily = (t.period ?? "day") === "day";
        const appliesToday = daily && active && (!t.day_profiles?.length || !todayProfile || t.day_profiles.includes(todayProfile));
        const p = appliesToday ? areas[t.area]?.targets?.find((x) => x.title === t.title && x.metric === t.metric) : undefined;
        const goal = t.goal_id ? byGoal.get(t.goal_id) : undefined;
        return (
          <Card key={t.id} id={`target-${t.id}`} style={areaVars(t.area)}
            className={cn("group relative overflow-hidden p-5 transition-colors hover:border-line-strong", !active && "border-dashed bg-transparent", highlight === t.id && "border-accent/60 shadow-[0_0_24px_-6px_rgb(34_211_238/.5)]")}>
            <div className="flex items-start gap-3">
              <span className="grid size-9 shrink-0 place-items-center rounded-xl bg-[color-mix(in_oklab,var(--a)_14%,transparent)] text-[var(--a)]"><a.icon className="size-4" aria-hidden /></span>
              <div className="min-w-0 flex-1">
                <button onClick={() => onEdit(t)} className="text-left text-[15px] font-semibold text-text hover:text-accent">{t.title}</button>
                <p className="text-[12.5px] text-text-3">{amountText(t)} a {daily ? "day" : "week"} · {a.label}</p>
              </div>
              {t.track_consistency ? <Flame className="size-4 shrink-0 text-warn" aria-label="Streak tracked" /> : null}
            </div>
            <div className="mt-4">
              {p ? (
                <>
                  <div className="mb-1.5 flex items-baseline justify-between text-[12.5px]">
                    <span className="text-text-2">Today</span>
                    <span className={cn("font-mono", p.achieved >= p.amount ? "text-good" : "text-text")}>{Math.round(p.achieved)} / {p.amount}</span>
                  </div>
                  <Bar value={p.amount ? p.achieved / p.amount : 0} color={p.achieved >= p.amount ? "var(--color-good)" : a.color} label={`${t.title} today: ${Math.round(p.achieved)} of ${p.amount}`} />
                </>
              ) : (
                <p className="text-[12.5px] text-text-3">
                  {!active
                    ? t.active_from && t.active_from > today ? `Starts ${fmtDay(t.active_from, "d MMM")}` : "Ended"
                    : !daily ? "Counted when the week closes" : appliesToday ? "No progress yet today" : `Not on ${todayProfile ?? "this"} days`}
                </p>
              )}
            </div>
            <div className="mt-3 flex flex-wrap gap-1.5">
              {t.day_profiles?.map((d) => <Badge key={d} className="capitalize">{d}</Badge>)}
              {t.match?.category ? <Badge>matches “{t.match.category}”</Badge> : null}
              {goal ? <Badge tone="accent"><Flag className="size-3" aria-hidden /> {HORIZON_META[goal.horizon].label}: {goal.title}</Badge> : null}
            </div>
          </Card>
        );
      })}
    </div>
  );
}

export function TargetSheet({ edit, goals, onClose }: { edit: Target | "new" | null; goals: Goal[]; onClose: () => void }) {
  const t = edit && edit !== "new" ? edit : undefined;
  const profiles = useDayProfiles();
  const names = [...new Set([...BUILTIN_PROFILES, ...(profiles.data?.items ?? []).map((p) => p.name).filter((n): n is string => !!n)])];
  const [title, setTitle] = useState(t?.title ?? "");
  const [area, setArea] = useState(t?.area ?? null);
  const [metric, setMetric] = useState<Target["metric"]>(t?.metric ?? "minutes");
  const [amount, setAmount] = useState(String(t?.amount ?? 30));
  const [period, setPeriod] = useState<"day" | "week">(t?.period ?? "day");
  const [days, setDays] = useState<Set<string>>(new Set(t?.day_profiles ?? []));
  const [goalId, setGoalId] = useState(t?.goal_id ?? "");
  const [category, setCategory] = useState(t?.match?.category ?? "");
  const [streak, setStreak] = useState(t?.track_consistency ?? false);
  const [from, setFrom] = useState(t?.active_from ?? "");
  const [to, setTo] = useState(t?.active_to ?? "");
  const save = useSaveTarget();
  const del = useDeleteTarget();
  const n = Number(amount);
  const badAmount = !Number.isFinite(n) || n <= 0;
  const badDates = !!from && !!to && to < from;
  const valid = title.trim() && area && !badAmount && !badDates;
  return (
    <Sheet
      open={!!edit}
      onOpenChange={(o) => !o && onClose()}
      title={t ? "Edit target" : "New target"}
      description="An amount to hit each day or week."
      footer={
        <>
          {t ? <DeleteButton loading={del.isPending} onDelete={() => del.mutate(t, { onSuccess: () => { toast("Target deleted"); onClose(); }, onError: (e) => toast.error(errText(e, "Couldn't delete")) })} /> : null}
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button type="submit" form="target-form" variant="primary" disabled={!valid} loading={save.isPending}>{t ? "Save" : "Add target"}</Button>
        </>
      }
    >
      <form
        id="target-form"
        className="space-y-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (!valid) return;
          save.mutate(
            {
              existing: t,
              body: {
                title: title.trim(), area: area!, metric, amount: n, period, day_profiles: [...days], goal_id: goalId || null,
                match: { category: category.trim() || null }, track_consistency: streak, active_from: from || null, active_to: to || null,
              },
            },
            { onSuccess: () => { toast.success(t ? "Target saved" : "Target added"); onClose(); }, onError: (err) => toast.error(errText(err)) },
          );
        }}
      >
        <Field label="Target" error={fieldErr(save.error, "title")}>
          {(f) => <Input {...f} autoFocus required maxLength={200} value={title} onChange={(e) => setTitle(e.target.value)} placeholder="Read" />}
        </Field>
        <AreaPicker value={area} onChange={setArea} />
        <div className="grid grid-cols-[1fr_1.4fr] gap-3">
          <Field label="Amount" error={badAmount ? "More than zero" : fieldErr(save.error, "amount")}>
            {(f) => <Input {...f} type="number" min={1} step="any" inputMode="decimal" value={amount} onChange={(e) => setAmount(e.target.value)} />}
          </Field>
          <Field label="Of">
            {(f) => (
              <NativeSelect {...f} value={metric} onChange={(e) => setMetric(e.target.value as Target["metric"])}>
                {METRICS.map((m) => <option key={m} value={m}>{m}</option>)}
              </NativeSelect>
            )}
          </Field>
        </div>
        <div>
          <p className="mb-2 text-[13px] font-medium text-text-2">Every</p>
          <Segmented<"day" | "week"> label="Period" value={period} onChange={setPeriod} options={[{ value: "day", label: "Day" }, { value: "week", label: "Week" }]} />
        </div>
        {period === "day" ? (
          <fieldset>
            <legend className="mb-2 text-[13px] font-medium text-text-2">On these kinds of day</legend>
            <div className="flex flex-wrap gap-1.5">
              {names.map((d) => (
                <Chip key={d} on={days.has(d)} onClick={() => setDays((s) => { const x = new Set(s); if (x.has(d)) x.delete(d); else x.add(d); return x; })}>
                  <span className="capitalize">{d}</span>
                </Chip>
              ))}
            </div>
            <p className="mt-1.5 text-[12.5px] text-text-3">None selected means every day.</p>
          </fieldset>
        ) : null}
        <Field label="Counts activity in category" hint="Optional. Only minutes detected or logged with this category count (minutes targets).">
          {(f) => <Input {...f} maxLength={80} value={category} onChange={(e) => setCategory(e.target.value)} placeholder="reading" />}
        </Field>
        <Field label="For goal">
          {(f) => (
            <NativeSelect {...f} value={goalId} onChange={(e) => setGoalId(e.target.value)}>
              <option value="">No goal</option>
              {goals.filter((g) => g.status !== "abandoned").map((g) => <option key={g.id} value={g.id}>{HORIZON_META[g.horizon].label}: {g.title}</option>)}
            </NativeSelect>
          )}
        </Field>
        <label className="flex cursor-pointer items-start gap-3 rounded-xl border border-line p-3">
          <input type="checkbox" className="mt-0.5 size-4 accent-[var(--color-accent)]" checked={streak} onChange={(e) => setStreak(e.target.checked)} />
          <span>
            <span className="flex items-center gap-1.5 text-sm text-text"><Flame className="size-4 text-warn" aria-hidden /> Track as a habit</span>
            <span className="text-[12.5px] text-text-3">Counts toward consistency: how often you hit it, not just today.</span>
          </span>
        </label>
        <div className="grid grid-cols-2 gap-3">
          <Field label="Active from">{(f) => <Input {...f} type="date" value={from} onChange={(e) => setFrom(e.target.value)} />}</Field>
          <Field label="Until" error={badDates ? "Ends before it starts" : undefined}>{(f) => <Input {...f} type="date" value={to} onChange={(e) => setTo(e.target.value)} />}</Field>
        </div>
      </form>
    </Sheet>
  );
}
