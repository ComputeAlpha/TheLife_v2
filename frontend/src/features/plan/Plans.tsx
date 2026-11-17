import { useState } from "react";
import { Eye, FileText, Flag, PencilLine, Plus } from "lucide-react";
import type { Goal, Plan } from "@/api/types";
import { useDeletePlan, useSavePlan } from "@/api/plan";
import { addDays, diffDays, fmtDay, startOfWeek, type Day } from "@/lib/calendar/days";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, NativeSelect, Textarea } from "@/ui/Field";
import { Markdown } from "@/ui/Markdown";
import { Segmented } from "@/ui/Segmented";
import { EmptyState } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { HORIZON_META } from "./Goals";
import { DeleteButton, errText, fieldErr, SectionLabel } from "./shared";

const STATUS_TONE = { draft: "neutral", active: "accent", done: "good", abandoned: "neutral" } as const;

function periodText(p: Plan) {
  const sameYear = p.period_start.slice(0, 4) === p.period_end.slice(0, 4);
  return `${fmtDay(p.period_start, sameYear ? "d MMM" : "d MMM yyyy")} – ${fmtDay(p.period_end, "d MMM yyyy")}`;
}

export function PlansView({ plans, goals, today, onEdit, highlight }: { plans: Plan[]; goals: Goal[]; today: Day; onEdit: (p: Plan | "new") => void; highlight: string | null }) {
  if (!plans.length) {
    return (
      <Card>
        <EmptyState
          art={<FileText className="size-10 text-accent" />}
          title="No plans yet"
          body="A plan is how a stretch of time should go: this week's focus, a sprint, a study block. Link it to a goal and the assistant plans with it."
          action={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => onEdit("new")}>Write a plan</Button>}
        />
      </Card>
    );
  }
  const byGoal = new Map(goals.map((g) => [g.id, g]));
  const sorted = [...plans].sort((a, b) => b.period_start.localeCompare(a.period_start));
  const now = sorted.filter((p) => p.period_start <= today && p.period_end >= today);
  const next = sorted.filter((p) => p.period_start > today).reverse();
  const past = sorted.filter((p) => p.period_end < today);
  const group = (label: string, list: Plan[]) =>
    list.length ? (
      <section key={label} aria-label={label}>
        <SectionLabel>{label}</SectionLabel>
        <div className="grid gap-4 md:grid-cols-2 2xl:grid-cols-3">
          {list.map((p) => <PlanCard key={p.id} p={p} goal={p.goal_id ? byGoal.get(p.goal_id) : undefined} today={today} onEdit={() => onEdit(p)} highlight={highlight === p.id} />)}
        </div>
      </section>
    ) : null;
  return <div>{group("Now", now)}{group("Upcoming", next)}{group("Past", past)}</div>;
}

function PlanCard({ p, goal, today, onEdit, highlight }: { p: Plan; goal?: Goal; today: Day; onEdit: () => void; highlight: boolean }) {
  const total = diffDays(p.period_end, p.period_start) + 1;
  const gone = Math.max(0, Math.min(total, diffDays(today, p.period_start) + 1));
  const status = p.status ?? "active";
  return (
    <Card id={`plan-${p.id}`} className={cn("group flex flex-col p-5 transition-colors hover:border-line-strong", highlight && "border-accent/60 shadow-[0_0_24px_-6px_rgb(34_211_238/.5)]")}>
      <div className="flex items-start gap-3">
        <div className="min-w-0 flex-1">
          <button onClick={onEdit} className="text-left text-[15px] font-semibold text-text hover:text-accent">{p.title}</button>
          <p className="mt-0.5 text-[12.5px] text-text-3">{periodText(p)}</p>
        </div>
        <Badge tone={STATUS_TONE[status]} className="capitalize">{status}</Badge>
      </div>
      {goal ? (
        <p className="mt-2 flex items-center gap-1.5 text-[12.5px] text-text-2">
          <Flag className="size-3.5 text-accent" aria-hidden />
          <span className="text-text-3">{HORIZON_META[goal.horizon].label}:</span> <span className="truncate">{goal.title}</span>
        </p>
      ) : null}
      {p.body ? (
        <div className="relative mt-3 max-h-40 overflow-hidden text-[13.5px] text-text-2 [mask-image:linear-gradient(to_bottom,black_70%,transparent)]">
          <Markdown>{p.body}</Markdown>
        </div>
      ) : (
        <p className="mt-3 text-[13px] italic text-text-3">No details yet.</p>
      )}
      {status === "active" && gone > 0 && gone < total ? (
        <div className="mt-auto pt-4">
          <div className="mb-1 flex justify-between text-[11.5px] text-text-3"><span>Day {gone} of {total}</span><span>{total - gone} left</span></div>
          <div className="h-1 overflow-hidden rounded-full bg-surface-3"><div className="h-full rounded-full bg-accent/70" style={{ width: `${(gone / total) * 100}%` }} /></div>
        </div>
      ) : null}
    </Card>
  );
}

export function PlanSheet({ edit, goals, today, onClose }: { edit: Plan | "new" | null; goals: Goal[]; today: Day; onClose: () => void }) {
  const p = edit && edit !== "new" ? edit : undefined;
  const monday = startOfWeek(today);
  const [title, setTitle] = useState(p?.title ?? "");
  const [goalId, setGoalId] = useState(p?.goal_id ?? "");
  const [start, setStart] = useState(p?.period_start ?? monday);
  const [end, setEnd] = useState(p?.period_end ?? addDays(monday, 6));
  const [status, setStatus] = useState(p?.status ?? "active");
  const [body, setBody] = useState(p?.body ?? "");
  const [tab, setTab] = useState<"write" | "preview">("write");
  const save = useSavePlan();
  const del = useDeletePlan();
  const badDates = !start || !end || end < start;
  const valid = title.trim() && !badDates;
  const openGoals = goals.filter((g) => g.status !== "abandoned");
  return (
    <Sheet
      open={!!edit}
      onOpenChange={(o) => !o && onClose()}
      title={p ? "Edit plan" : "New plan"}
      description="How a stretch of time should go."
      className="max-w-[560px]"
      footer={
        <>
          {p ? <DeleteButton loading={del.isPending} onDelete={() => del.mutate(p, { onSuccess: () => { toast("Plan deleted"); onClose(); }, onError: (e) => toast.error(errText(e, "Couldn't delete")) })} /> : null}
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button type="submit" form="plan-form" variant="primary" disabled={!valid} loading={save.isPending}>{p ? "Save" : "Add plan"}</Button>
        </>
      }
    >
      <form
        id="plan-form"
        className="space-y-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (!valid) return;
          save.mutate(
            { existing: p, body: { title: title.trim(), goal_id: goalId || null, period_start: start, period_end: end, status, body: body.trim() || null } },
            { onSuccess: () => { toast.success(p ? "Plan saved" : "Plan added"); onClose(); }, onError: (err) => toast.error(errText(err)) },
          );
        }}
      >
        <Field label="Title" error={fieldErr(save.error, "title")}>
          {(f) => <Input {...f} autoFocus required maxLength={200} value={title} onChange={(e) => setTitle(e.target.value)} placeholder="Release week" />}
        </Field>
        <div className="grid grid-cols-2 gap-3">
          <Field label="From">{(f) => <Input {...f} type="date" required value={start} onChange={(e) => setStart(e.target.value)} />}</Field>
          <Field label="To" error={badDates ? "Ends before it starts" : undefined}>{(f) => <Input {...f} type="date" required value={end} onChange={(e) => setEnd(e.target.value)} />}</Field>
        </div>
        <Field label="For goal">
          {(f) => (
            <NativeSelect {...f} value={goalId} onChange={(e) => setGoalId(e.target.value)}>
              <option value="">No goal</option>
              {openGoals.map((g) => <option key={g.id} value={g.id}>{HORIZON_META[g.horizon].label}: {g.title}</option>)}
            </NativeSelect>
          )}
        </Field>
        <Field label="Status">
          {(f) => (
            <NativeSelect {...f} value={status} onChange={(e) => setStatus(e.target.value as typeof status)}>
              <option value="draft">Draft</option>
              <option value="active">Active</option>
              <option value="done">Done</option>
              <option value="abandoned">Abandoned</option>
            </NativeSelect>
          )}
        </Field>
        <div>
          <div className="mb-2 flex items-center justify-between">
            <p className="text-[13px] font-medium text-text-2">Plan</p>
            <Segmented label="Editor" value={tab} onChange={setTab} className="p-0.5"
              options={[{ value: "write", label: <span className="flex items-center gap-1 text-[12.5px]"><PencilLine className="size-3.5" />Write</span> }, { value: "preview", label: <span className="flex items-center gap-1 text-[12.5px]"><Eye className="size-3.5" />Preview</span> }]} />
          </div>
          {tab === "write" ? (
            <Textarea aria-label="Plan body (Markdown)" rows={12} value={body} onChange={(e) => setBody(e.target.value)} className="font-mono text-[13px]"
              placeholder={"## Focus\n- Finish the release notes\n- **Two** deep-work blocks a day"} />
          ) : (
            <div className="min-h-48 rounded-lg border border-line p-4 text-sm text-text-2">{body.trim() ? <Markdown>{body}</Markdown> : <p className="text-text-3">Nothing to preview.</p>}</div>
          )}
        </div>
      </form>
    </Sheet>
  );
}
