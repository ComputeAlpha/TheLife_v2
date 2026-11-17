import { useState } from "react";
import { Check, Flame, SkipForward } from "lucide-react";
import type { Area, Task } from "@/api/types";
import { useDeleteTask, useOccurrenceAction, useSaveTask, useTaskOccurrences } from "@/api/tasks";
import { useGoals } from "@/api/plan";
import { useProjects } from "@/api/work";
import { BUILTIN_PROFILES, useDayProfiles } from "@/api/calendar";
import { addDays, dayOf, fmtDay, type Day } from "@/lib/calendar/days";
import { fromLocalInput, toLocalInput } from "@/lib/time";
import { PRIORITY } from "@/lib/tasks";
import { AreaPicker } from "@/ui/AreaPicker";
import { Button } from "@/ui/Button";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, NativeSelect, Textarea } from "@/ui/Field";
import { Chip, Segmented } from "@/ui/Segmented";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { RecurrenceBuilder } from "../common/RecurrenceBuilder";
import { DeleteButton, errText, fieldErr } from "../plan/shared";

export function TaskSheet({ task, open, onClose, tz, today, defaults }: {
  task: Task | null; open: boolean; onClose: () => void; tz: string; today: Day; defaults?: { goal_id?: string; project_id?: string };
}) {
  const t = task ?? undefined;
  const profiles = useDayProfiles();
  const goals = useGoals();
  const projects = useProjects();
  const names = [...new Set([...BUILTIN_PROFILES, ...(profiles.data?.items ?? []).map((p) => p.name).filter((n): n is string => !!n)])];
  const [title, setTitle] = useState(t?.title ?? "");
  const [area, setArea] = useState<Area | null>(t?.area ?? null);
  const [priority, setPriority] = useState(String(t?.priority ?? 1));
  const [due, setDue] = useState(t?.due_at ? toLocalInput(t.due_at, tz) : "");
  const [rrule, setRrule] = useState<string | null>(t?.rrule ?? null);
  const [start, setStart] = useState(toLocalInput(t?.rrule_dtstart ?? `${today}T03:30:00Z`, tz));
  const [estimate, setEstimate] = useState(t?.estimate_min ? String(t.estimate_min) : "");
  const [goalId, setGoalId] = useState(t?.goal_id ?? defaults?.goal_id ?? "");
  const [projectId, setProjectId] = useState(t?.project_id ?? defaults?.project_id ?? "");
  const [days, setDays] = useState<Set<string>>(new Set(t?.day_profiles ?? []));
  const [streak, setStreak] = useState(t?.track_consistency ?? false);
  const [notes, setNotes] = useState(t?.notes ?? "");
  const save = useSaveTask();
  const del = useDeleteTask();
  const dueIso = due ? fromLocalInput(due, tz) : null;
  const startIso = fromLocalInput(start, tz);
  const startDay = startIso ? dayOf(Date.parse(startIso), tz) : today;
  const valid = title.trim() && area && (!rrule || startIso);

  return (
    <Sheet
      open={open}
      onOpenChange={(o) => !o && onClose()}
      title={t ? "Edit task" : "New task"}
      description={t?.rrule ? "A repeating task: each day it falls on is an occurrence." : "Something to get done."}
      className="max-w-[540px]"
      footer={
        <>
          {t ? <DeleteButton loading={del.isPending} onDelete={() => del.mutate(t, { onSuccess: () => { toast("Task deleted"); onClose(); }, onError: (e) => toast.error(errText(e, "Couldn't delete")) })} /> : null}
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button type="submit" form="task-form" variant="primary" disabled={!valid} loading={save.isPending}>{t ? "Save" : "Add task"}</Button>
        </>
      }
    >
      <form
        id="task-form"
        className="space-y-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (!valid) return;
          save.mutate(
            {
              existing: t,
              body: {
                title: title.trim(), area: area!, priority: Number(priority), notes: notes.trim() || null,
                estimate_min: estimate ? Math.max(1, Number(estimate)) : null,
                goal_id: goalId || null, project_id: projectId || null,
                rrule, rrule_dtstart: rrule ? startIso : null, due_at: rrule ? null : dueIso,
                day_profiles: rrule ? [...days] : [], track_consistency: rrule ? streak : false,
              },
            },
            { onSuccess: () => { toast.success(t ? "Task saved" : "Task added"); onClose(); }, onError: (err) => toast.error(errText(err)) },
          );
        }}
      >
        <Field label="Task" error={fieldErr(save.error, "title")}>
          {(f) => <Input {...f} autoFocus required maxLength={200} value={title} onChange={(e) => setTitle(e.target.value)} placeholder="Send the weekly report" />}
        </Field>
        <AreaPicker value={area} onChange={setArea} />
        <div>
          <p className="mb-2 text-[13px] font-medium text-text-2">Priority</p>
          <Segmented label="Priority" value={priority} onChange={setPriority} options={[0, 1, 2, 3].map((p) => ({ value: String(p), label: PRIORITY[p]!.label }))} />
        </div>
        <RecurrenceBuilder value={rrule} onChange={setRrule} startDay={startDay} today={today} />
        <div className="grid grid-cols-2 gap-3">
          {rrule ? (
            <Field label="Starts" hint="First day, and the time of day it's due.">{(f) => <Input {...f} type="datetime-local" value={start} onChange={(e) => setStart(e.target.value)} />}</Field>
          ) : (
            <Field label="Due" hint="Optional." error={fieldErr(save.error, "due_at")}>{(f) => <Input {...f} type="datetime-local" value={due} onChange={(e) => setDue(e.target.value)} />}</Field>
          )}
          <Field label="Estimate (min)">{(f) => <Input {...f} type="number" min={1} value={estimate} onChange={(e) => setEstimate(e.target.value)} />}</Field>
        </div>
        {rrule ? (
          <>
            <fieldset>
              <legend className="mb-2 text-[13px] font-medium text-text-2">Only on these kinds of day</legend>
              <div className="flex flex-wrap gap-1.5">
                {names.map((d) => (
                  <Chip key={d} on={days.has(d)} onClick={() => setDays((s) => { const x = new Set(s); if (x.has(d)) x.delete(d); else x.add(d); return x; })}><span className="capitalize">{d}</span></Chip>
                ))}
              </div>
              <p className="mt-1.5 text-[12.5px] text-text-3">None selected means every day the rule falls on.</p>
            </fieldset>
            <label className="flex cursor-pointer items-center gap-3 rounded-xl border border-line p-3 text-sm text-text">
              <input type="checkbox" className="size-4 accent-[var(--color-accent)]" checked={streak} onChange={(e) => setStreak(e.target.checked)} />
              <Flame className="size-4 text-warn" aria-hidden /> Track as a habit (consistency)
            </label>
          </>
        ) : null}
        <div className="grid grid-cols-2 gap-3">
          <Field label="Goal">
            {(f) => (
              <NativeSelect {...f} value={goalId} onChange={(e) => setGoalId(e.target.value)}>
                <option value="">None</option>
                {(goals.data ?? []).filter((g) => g.status !== "abandoned").map((g) => <option key={g.id} value={g.id}>{g.title}</option>)}
              </NativeSelect>
            )}
          </Field>
          <Field label="Project">
            {(f) => (
              <NativeSelect {...f} value={projectId} onChange={(e) => setProjectId(e.target.value)}>
                <option value="">None</option>
                {(projects.data ?? []).map((p) => <option key={p.id} value={p.id}>{p.title}</option>)}
              </NativeSelect>
            )}
          </Field>
        </div>
        <Field label="Notes">{(f) => <Textarea {...f} rows={3} value={notes} onChange={(e) => setNotes(e.target.value)} />}</Field>
      </form>
      {t?.rrule ? <OccurrenceStrip task={t} today={today} /> : null}
    </Sheet>
  );
}

const OCC_STYLE: Record<string, string> = {
  done: "border-good/50 bg-good/15 text-good",
  skipped: "border-line bg-surface-2 text-text-3 line-through",
  missed: "border-alert/50 bg-alert/10 text-alert-text",
  todo: "border-line-strong text-text-2",
};

/** The last and next 14 days of a repeating task, each completable or skippable once it has come. */
function OccurrenceStrip({ task, today }: { task: Task; today: Day }) {
  const from = addDays(today, -14);
  const to = addDays(today, 14);
  const q = useTaskOccurrences(task.id, from, to);
  const act = useOccurrenceAction();
  const [picked, setPicked] = useState<Day | null>(null);
  const items = q.data?.items ?? [];
  const sel = items.find((o) => o.occurs_on === picked);
  return (
    <section aria-label="Occurrences" className="mt-6 border-t border-line pt-4">
      <h3 className="mb-1 text-[12px] font-semibold uppercase tracking-wider text-text-3">Occurrences</h3>
      <p className="mb-3 text-[12.5px] text-text-3">Two weeks back and ahead. Pick a day to complete or skip it.</p>
      {!items.length && !q.isPending ? <p className="text-[13px] text-text-3">None in these four weeks.</p> : null}
      <ol className="flex flex-wrap gap-1.5">
        {items.map((o) => {
          const future = o.occurs_on > today;
          return (
            <li key={o.occurs_on}>
              <button
                type="button"
                disabled={future}
                onClick={() => setPicked(o.occurs_on === picked ? null : o.occurs_on)}
                aria-pressed={picked === o.occurs_on}
                aria-label={`${fmtDay(o.occurs_on, "EEEE d MMMM")}: ${o.status}`}
                className={cn("flex w-11 flex-col items-center rounded-lg border py-1 text-[11px] leading-tight transition-colors",
                  OCC_STYLE[o.status], future && "border-dashed opacity-100 text-text-3", o.occurs_on === today && "ring-1 ring-accent", picked === o.occurs_on && "shadow-[0_0_0_1.5px_var(--color-accent)]")}
              >
                <span className="text-[10px] uppercase">{fmtDay(o.occurs_on, "EEE")}</span>
                <span className="font-mono text-[12.5px]">{Number(o.occurs_on.slice(8))}</span>
              </button>
            </li>
          );
        })}
      </ol>
      {sel ? (
        <div className="mt-3 flex flex-wrap items-center gap-2 rounded-xl border border-line p-3">
          <span className="mr-auto text-[13px] text-text-2">{fmtDay(sel.occurs_on, "EEEE d MMM")} · <span className="capitalize">{sel.status}</span>{sel.skip_reason ? ` (“${sel.skip_reason}”)` : ""}</span>
          {sel.status !== "done" ? (
            <Button size="sm" variant="primary" icon={<Check className="size-3.5" />} loading={act.isPending && act.variables?.verb === "complete"}
              onClick={() => act.mutate({ id: task.id, date: sel.occurs_on, verb: "complete" }, { onSuccess: () => toast.success("Done"), onError: (e) => toast.error(errText(e)) })}>Complete</Button>
          ) : null}
          {sel.status !== "skipped" ? (
            <Button size="sm" icon={<SkipForward className="size-3.5" />} loading={act.isPending && act.variables?.verb === "skip"}
              onClick={() => act.mutate({ id: task.id, date: sel.occurs_on, verb: "skip" }, { onSuccess: () => toast("Skipped"), onError: (e) => toast.error(errText(e)) })}>Skip</Button>
          ) : null}
        </div>
      ) : null}
    </section>
  );
}
