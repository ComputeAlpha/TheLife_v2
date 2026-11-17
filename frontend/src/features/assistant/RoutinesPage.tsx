import { useState } from "react";
import { CalendarDays, Moon, Plus, SkipForward, Sparkles, Sunrise } from "lucide-react";
import { useUser } from "@/api/auth";
import { BUILTIN_PROFILES, useDayProfiles } from "@/api/calendar";
import { useDeleteRoutine, useRoutineList, useSaveRoutine, useSkipRoutine } from "@/api/assistant";
import type { Routine } from "@/api/types";
import { WEEKDAY_CODES } from "@/lib/calendar/days";
import { localDay } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Markdown } from "@/ui/Markdown";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { Switch } from "@/ui/Switch";
import { FormSheet, opt } from "../common/FormSheet";
import { OriginBadge } from "../common/OriginBadge";
import { errText } from "../plan/shared";

const KIND = { morning: { label: "Morning", icon: Sunrise }, evening: { label: "Evening", icon: Moon }, weekly: { label: "Weekly", icon: CalendarDays }, custom: { label: "Custom", icon: Sparkles } } as const;
const WD = { MO: "Mon", TU: "Tue", WE: "Wed", TH: "Thu", FR: "Fri", SA: "Sat", SU: "Sun" } as const;


export function RoutinesPage() {
  const user = useUser();
  const today = localDay(useNow(), user.timezone, user.day_start_hour);
  const q = useRoutineList();
  const save = useSaveRoutine();
  const skip = useSkipRoutine();
  const [edit, setEdit] = useState<Routine | "new" | null>(null);
  const list = [...(q.data ?? [])].sort((a, b) => Number(b.enabled ?? true) - Number(a.enabled ?? true) || (a.learned_time ?? a.default_time).localeCompare(b.learned_time ?? b.default_time));
  return (
    <>
      <div className="mb-4 flex items-center justify-between gap-3">
        <p className="text-sm text-text-2">Check-ins the assistant runs for you: a morning plan, an evening review, a weekly look back.</p>
        <Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setEdit("new")}>New routine</Button>
      </div>
      {q.isPending ? <div className="grid gap-4 md:grid-cols-2">{[0, 1].map((i) => <Skeleton key={i} className="h-44" />)}</div> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : !list.length ? (
        <Card><EmptyState art={<Sunrise className="size-10 text-accent" />} title="No routines yet" body="A morning routine plans your day with you; an evening one reviews it." action={<Button variant="primary" onClick={() => setEdit("new")}>Add a routine</Button>} /></Card>
      ) : (
        <ul className="grid gap-4 md:grid-cols-2 2xl:grid-cols-3">
          {list.map((r) => {
            const K = KIND[r.kind];
            const enabled = r.enabled ?? true;
            const skipped = r.skip_on === today;
            return (
              <li key={r.id}>
                <Card className={cn("flex h-full flex-col p-5", !enabled && "border-dashed bg-transparent")}>
                  <div className="flex items-start gap-3">
                    <span className="grid size-10 shrink-0 place-items-center rounded-xl bg-accent/10 text-accent"><K.icon className="size-5" aria-hidden /></span>
                    <div className="min-w-0 flex-1">
                      <button onClick={() => setEdit(r)} className="text-left text-[15px] font-semibold text-text hover:text-accent">{r.title}</button>
                      <p className="text-[12.5px] text-text-3">{K.label} · {r.mode === "fixed" ? "Fixed time" : "Adapts to you"}</p>
                    </div>
                    <Switch on={enabled} label={`${r.title} enabled`} disabled={save.isPending}
                      onChange={(v) => save.mutate({ existing: r, body: { enabled: v } }, { onSuccess: () => toast(v ? "Routine on" : "Routine off"), onError: (e) => toast.error(errText(e)) })} />
                  </div>
                  <div className="mt-4 flex items-baseline gap-3">
                    <span className="font-display text-3xl font-semibold tabular-nums text-text">{r.mode !== "fixed" && r.learned_time ? r.learned_time : r.default_time}</span>
                    <span className="text-[12.5px] text-text-3">
                      {r.mode === "fixed" ? "every time" : r.learned_time ? `learned · default ${r.default_time}` : "default, until it learns"}
                      {r.mode !== "fixed" && r.window_start && r.window_end ? ` · window ${r.window_start}–${r.window_end}` : ""}
                    </span>
                  </div>
                  <div className="mt-3 flex flex-wrap gap-1">
                    {WEEKDAY_CODES.map((d) => (
                      <span key={d} className={cn("rounded-md px-1.5 py-0.5 text-[11px]", !r.weekdays?.length || r.weekdays.includes(d) ? "bg-surface-3 text-text-2" : "text-text-3 line-through")}>{WD[d]}</span>
                    ))}
                    {r.day_profiles?.map((p) => <Badge key={p} className="capitalize">{p}</Badge>)}
                  </div>
                  {r.instructions ? <div className="mt-3 line-clamp-4 text-[13px] text-text-2"><Markdown>{r.instructions}</Markdown></div> : null}
                  <div className="mt-auto flex items-center gap-2 pt-4">
                    {enabled ? (
                      skipped ? <Badge tone="warn">Skipped today</Badge> : (
                        <Button size="sm" variant="ghost" icon={<SkipForward className="size-3.5" />} loading={skip.isPending && skip.variables?.id === r.id}
                          onClick={() => skip.mutate({ id: r.id, date: today }, { onSuccess: () => toast("Skipped for today"), onError: (e) => toast.error(errText(e)) })}>Skip today</Button>
                      )
                    ) : null}
                    {r.last_fired_on ? <span className="ml-auto text-[12px] text-text-3">last ran {r.last_fired_on === today ? "today" : r.last_fired_on}</span> : null}
                    <OriginBadge row={r} />
                  </div>
                </Card>
              </li>
            );
          })}
        </ul>
      )}
      {edit ? <RoutineSheet existing={edit === "new" ? undefined : edit} onClose={() => setEdit(null)} /> : null}
    </>
  );
}

function RoutineSheet({ existing, onClose }: { existing?: Routine; onClose: () => void }) {
  const save = useSaveRoutine();
  const del = useDeleteRoutine();
  const profiles = useDayProfiles();
  const names = [...new Set([...BUILTIN_PROFILES, ...(profiles.data?.items ?? []).map((p) => p.name).filter((n): n is string => !!n)])];
  const r = existing;
  return (
    <FormSheet
      id="routine-form"
      title={r ? "Edit routine" : "New routine"}
      description="The assistant runs it on the days you pick and sends what it prepares to chat and Telegram."
      submitLabel={r ? "Save" : "Add routine"}
      onClose={onClose}
      initial={{
        kind: r?.kind ?? "morning", title: r?.title ?? "", mode: r?.mode ?? "adaptive", default_time: r?.default_time ?? "07:30",
        window_start: r?.window_start ?? "", window_end: r?.window_end ?? "", weekdays: r?.weekdays ?? [], day_profiles: r?.day_profiles ?? [],
        instructions: r?.instructions ?? "", enabled: r?.enabled ?? true,
      }}
      validate={(v): Record<string, string> => (v.mode === "adaptive" && (!!v.window_start !== !!v.window_end) ? { window_end: "Give both ends of the window, or neither" } : v.window_start && v.window_end && String(v.window_end) <= String(v.window_start) ? { window_end: "Ends before it starts" } : {})}
      fields={[
        { key: "kind", label: "Kind", type: "segmented", options: (Object.keys(KIND) as (keyof typeof KIND)[]).map((k) => ({ value: k, label: KIND[k].label })) },
        { key: "title", label: "Name", type: "text", required: true, maxLength: 200, placeholder: "Morning plan" },
        { key: "mode", label: "Timing", type: "segmented", options: [{ value: "adaptive", label: "Adapts to me" }, { value: "fixed", label: "Fixed time" }] },
        { key: "default_time", label: "Time", type: "time", required: true, hint: "Adaptive routines start here and learn from when you actually respond." },
        { key: "window_start", label: "Earliest (adaptive)", type: "time" },
        { key: "window_end", label: "Latest (adaptive)", type: "time" },
        { key: "weekdays", label: "On", type: "chips", options: WEEKDAY_CODES.map((d) => ({ value: d, label: WD[d] })), hint: "None selected means every day." },
        { key: "day_profiles", label: "Only on these kinds of day", type: "chips", options: names.map((n) => ({ value: n, label: n })), hint: "None selected means any kind of day." },
        { key: "instructions", label: "What it should do", type: "textarea", placeholder: "Plan the day around my deep-work blocks, and remind me about anything due." },
        { key: "enabled", label: "Enabled", type: "checkbox" },
      ]}
      onSubmit={(v) => save.mutateAsync({
        existing: r,
        body: {
          kind: v.kind as Routine["kind"], title: String(v.title).trim(), mode: v.mode as Routine["mode"], default_time: String(v.default_time),
          window_start: v.mode === "adaptive" ? opt(v.window_start) : null, window_end: v.mode === "adaptive" ? opt(v.window_end) : null,
          weekdays: v.weekdays as NonNullable<Routine["weekdays"]>, day_profiles: v.day_profiles as string[], instructions: opt(v.instructions), enabled: !!v.enabled,
        },
      })}
      onDelete={r ? () => del.mutateAsync(r) : undefined}
    />
  );
}
