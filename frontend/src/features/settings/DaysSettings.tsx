import { useMemo, useState } from "react";
import { CalendarCog, Plus, Scale, Trash2 } from "lucide-react";
import { useUser } from "@/api/auth";
import { BUILTIN_PROFILES, useDayProfiles, useOverrides, useSetOverride } from "@/api/calendar";
import { useChangeOverride, useDeleteDayProfile, useSaveDayProfile, useSaveScoringSettings, useScoringSettings } from "@/api/settings";
import type { DayProfile, ScoringSettings } from "@/api/types";
import { areaMeta } from "@/lib/areas";
import { addDays, fmtDay, isValidDay } from "@/lib/calendar/days";
import { localDay } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button, IconButton } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { Field, Input, NativeSelect } from "@/ui/Field";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { FormSheet, opt } from "../common/FormSheet";
import { errText } from "../plan/shared";

const BUILTIN_NOTE: Record<string, string> = {
  workday: "A normal working day.",
  weekend: "Your days off in the week.",
  holiday: "Public holidays: not scored.",
  leave: "Time off: not scored, streaks pause.",
  sick: "Sick days: not scored, streaks pause.",
};

export function DaysSettings() {
  return (
    <div className="space-y-6">
      <ProfilesCard />
      <OverridesCard />
      <WeightsCard />
    </div>
  );
}

function ProfilesCard() {
  const q = useDayProfiles();
  const [edit, setEdit] = useState<DayProfile | "new" | null>(null);
  const custom = (q.data?.items ?? []).filter((p) => !BUILTIN_PROFILES.includes(p.name));
  return (
    <Card>
      <CardHeader title="Kinds of day" icon={<CalendarCog className="size-4" />} subtitle="Each day has one. It decides whether the day is scored and which routines and targets apply." action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setEdit("new")}>New kind</Button>} />
      <div className="px-5 pb-5">
        {q.isPending ? <Skeleton className="h-24" /> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : (
          <ul className="divide-y divide-line">
            {BUILTIN_PROFILES.map((n) => (
              <li key={n} className="flex items-center gap-3 py-2.5">
                <span className="w-24 text-sm capitalize text-text">{n}</span>
                <span className="flex-1 text-[12.5px] text-text-3">{BUILTIN_NOTE[n]}</span>
                <Badge>Built in</Badge>
              </li>
            ))}
            {custom.map((p) => (
              <li key={p.id}>
                <button onClick={() => setEdit(p)} className="flex w-full items-center gap-3 py-2.5 text-left">
                  <span className="w-24 text-sm text-text">{p.name}</span>
                  <span className="flex-1 truncate text-[12.5px] text-text-3">{p.description ?? ""}</span>
                  <Badge tone={p.scored === false ? "neutral" : "accent"}>{p.scored === false ? "Not scored" : "Scored"}</Badge>
                  {p.freezes_streaks ? <Badge tone="warn">Pauses streaks</Badge> : null}
                </button>
              </li>
            ))}
          </ul>
        )}
      </div>
      {edit ? <ProfileSheet existing={edit === "new" ? undefined : edit} onClose={() => setEdit(null)} /> : null}
    </Card>
  );
}

function ProfileSheet({ existing, onClose }: { existing?: DayProfile; onClose: () => void }) {
  const save = useSaveDayProfile();
  const del = useDeleteDayProfile();
  return (
    <FormSheet
      id="profile-form"
      title={existing ? `Edit “${existing.name}”` : "New kind of day"}
      submitLabel={existing ? "Save" : "Add"}
      onClose={onClose}
      initial={{ name: existing?.name ?? "", scored: existing?.scored ?? true, freezes_streaks: existing?.freezes_streaks ?? false, description: existing?.description ?? "" }}
      validate={(v): Record<string, string> => {
        const n = String(v.name).trim().toLowerCase();
        return BUILTIN_PROFILES.includes(n) ? { name: "That's a built-in kind" } : n && !/^[a-z0-9_-]+$/.test(n) ? { name: "Lowercase letters, digits, - and _" } : {};
      }}
      fields={[
        { key: "name", label: "Name", type: "text", required: true, maxLength: 40, placeholder: "travel", hint: "Lowercase, like travel or deep-work." },
        { key: "scored", label: "Scored", type: "checkbox", hint: "Off for days that shouldn't count, like travel." },
        { key: "freezes_streaks", label: "Pauses streaks", type: "checkbox", hint: "Habits aren't broken by this day." },
        { key: "description", label: "Description", type: "textarea" },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing, body: { name: String(v.name).trim().toLowerCase(), scored: !!v.scored, freezes_streaks: !!v.freezes_streaks, description: opt(v.description) } })}
      onDelete={existing ? () => del.mutateAsync(existing) : undefined}
    />
  );
}

function OverridesCard() {
  const user = useUser();
  const today = localDay(useNow(), user.timezone, user.day_start_hour);
  const to = addDays(today, 90);
  const q = useOverrides(today, to);
  const profiles = useDayProfiles();
  const set = useSetOverride();
  const change = useChangeOverride();
  const names = useMemo(() => [...new Set([...BUILTIN_PROFILES, ...(profiles.data?.items ?? []).map((p) => p.name)])], [profiles.data]);
  const [day, setDay] = useState(addDays(today, 1));
  const [profile, setProfile] = useState("leave");
  const list = [...(q.data?.items ?? [])].sort((a, b) => a.local_day.localeCompare(b.local_day));
  return (
    <Card>
      <CardHeader title="Planned exceptions" icon={<CalendarCog className="size-4" />} subtitle="Days that aren't their usual kind, for the next three months. The calendar's day menu sets these too." />
      <div className="px-5 pb-5">
        <form className="mb-4 flex flex-wrap items-end gap-2" onSubmit={(e) => {
          e.preventDefault();
          if (!isValidDay(day)) return;
          const existing = list.find((o) => o.local_day === day);
          (existing ? change.mutateAsync({ o: existing, profile }) : set.mutateAsync({ day, profile }))
            .then(() => toast.success(`${fmtDay(day, "EEE d MMM")} is now a ${profile} day`), (err) => toast.error(errText(err)));
        }}>
          <Field label="Day">{(f) => <Input {...f} type="date" min={today} value={day} onChange={(e) => setDay(e.target.value)} />}</Field>
          <Field label="Kind">{(f) => <NativeSelect {...f} value={profile} onChange={(e) => setProfile(e.target.value)}>{names.map((n) => <option key={n} value={n}>{n}</option>)}</NativeSelect>}</Field>
          <Button type="submit" loading={set.isPending || change.isPending}>Set</Button>
        </form>
        {q.isPending ? <Skeleton className="h-12" /> : !list.length ? <EmptyState className="py-4" title="No exceptions planned" /> : (
          <ul className="divide-y divide-line" aria-label="Planned exceptions">
            {list.map((o) => (
              <li key={o.id} className="flex items-center gap-3 py-2">
                <span className="w-32 text-sm text-text">{fmtDay(o.local_day, "EEE d MMM")}</span>
                <NativeSelect aria-label={`Kind of ${fmtDay(o.local_day, "d MMM")}`} className="h-8 w-40 text-[13px]" value={o.profile}
                  onChange={(e) => change.mutate({ o, profile: e.target.value }, { onSuccess: () => toast("Changed"), onError: (err) => toast.error(errText(err)) })}>
                  {names.map((n) => <option key={n} value={n}>{n}</option>)}
                </NativeSelect>
                <IconButton size="sm" className="ml-auto" label={`Remove the exception on ${fmtDay(o.local_day, "d MMM")}`}
                  onClick={() => set.mutate({ day: o.local_day, profile: null, existing: { id: o.id, version: o.version } }, { onSuccess: () => toast("Back to usual"), onError: (err) => toast.error(errText(err)) })}>
                  <Trash2 className="size-4" />
                </IconButton>
              </li>
            ))}
          </ul>
        )}
      </div>
    </Card>
  );
}

const W_KEYS = ["work", "skill", "fitness", "personal", "consistency"] as const;
const A_KEYS = ["work", "skill", "fitness", "personal"] as const;

function WeightsCard() {
  const q = useScoringSettings();
  if (q.isPending) return <Skeleton className="h-64" />;
  if (q.error) return <ErrorState error={q.error} retry={() => q.refetch()} />;
  return <WeightsForm key={JSON.stringify(q.data)} initial={q.data} />;
}

function WeightsForm({ initial }: { initial: ScoringSettings }) {
  const save = useSaveScoringSettings();
  const [w, setW] = useState<Record<string, number>>(() => Object.fromEntries(W_KEYS.map((k) => [k, initial.weights[k] ?? 0])));
  const [ps, setPs] = useState<Record<string, number>>(() => Object.fromEntries(A_KEYS.map((k) => [k, initial.plan_share[k] ?? 0.8])));
  const sum = W_KEYS.reduce((s, k) => s + (w[k] ?? 0), 0);
  const color = (k: string) => (k === "consistency" ? "var(--color-warn)" : areaMeta(k).color);
  const label = (k: string) => (k === "consistency" ? "Consistency" : areaMeta(k).label);
  const dirty = W_KEYS.some((k) => w[k] !== (initial.weights[k] ?? 0)) || A_KEYS.some((k) => ps[k] !== (initial.plan_share[k] ?? 0.8));
  return (
    <Card>
      <CardHeader title="Scoring" icon={<Scale className="size-4" />} subtitle="How much each area counts, and how much of an area is sticking to the plan versus hitting targets." />
      <div className="space-y-6 px-5 pb-5">
        <div>
          <p className="mb-2 text-[13px] font-medium text-text-2">Area weights</p>
          <div className="mb-3 flex h-3 gap-[2px] overflow-hidden rounded-full" role="img" aria-label={`Shares: ${W_KEYS.map((k) => `${label(k)} ${sum ? Math.round((w[k]! / sum) * 100) : 0}%`).join(", ")}`}>
            {W_KEYS.filter((k) => w[k]! > 0).map((k) => <div key={k} style={{ flexGrow: w[k], background: color(k) }} className="h-full first:rounded-l-full last:rounded-r-full transition-[flex-grow]" />)}
          </div>
          <ul className="space-y-2.5">
            {W_KEYS.map((k) => (
              <li key={k} className="grid grid-cols-[110px_1fr_52px] items-center gap-3">
                <label htmlFor={`w-${k}`} className="text-sm text-text">{label(k)}</label>
                <input id={`w-${k}`} type="range" min={0} max={1} step={0.05} value={w[k]} onChange={(e) => setW((x) => ({ ...x, [k]: Number(e.target.value) }))} className="w-full" style={{ accentColor: color(k) }} />
                <span className="text-right font-mono text-[12.5px] text-text-2">{sum ? Math.round((w[k]! / sum) * 100) : 0}%</span>
              </li>
            ))}
          </ul>
          <p className="mt-2 text-[12.5px] text-text-3">Weights are relative: the percentages are each area's share. Areas with nothing planned on a day drop out and the rest share their weight.</p>
        </div>
        <div>
          <p className="mb-2 text-[13px] font-medium text-text-2">Plan versus targets</p>
          <ul className="space-y-2.5">
            {A_KEYS.map((k) => (
              <li key={k} className="grid grid-cols-[96px_1fr_auto] items-center gap-3">
                <label htmlFor={`p-${k}`} className="text-sm text-text">{label(k)}</label>
                <input id={`p-${k}`} type="range" min={0} max={1} step={0.05} value={ps[k]} onChange={(e) => setPs((x) => ({ ...x, [k]: Number(e.target.value) }))} className="w-full" style={{ accentColor: color(k) }} />
                <span className="whitespace-nowrap text-right font-mono text-[12px] text-text-2">{Math.round(ps[k]! * 100)}% plan · {Math.round((1 - ps[k]!) * 100)}% targets</span>
              </li>
            ))}
          </ul>
        </div>
        <div className="flex justify-end gap-2 border-t border-line pt-4">
          <Button variant="ghost" disabled={!dirty} onClick={() => { setW(Object.fromEntries(W_KEYS.map((k) => [k, initial.weights[k] ?? 0]))); setPs(Object.fromEntries(A_KEYS.map((k) => [k, initial.plan_share[k] ?? 0.8]))); }}>Reset</Button>
          <Button variant="primary" disabled={!dirty || sum === 0} loading={save.isPending}
            onClick={() => save.mutate({ weights: w, plan_share: ps }, { onSuccess: () => toast.success("Scoring saved", "It applies from the next day scored."), onError: (e) => toast.error(errText(e)) })}>
            Save
          </Button>
        </div>
      </div>
    </Card>
  );
}
