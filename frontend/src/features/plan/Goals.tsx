import { useMemo, useState } from "react";
import { CalendarClock, ChevronRight, FileText, Flag, ListTodo, Mountain, Plus, Target as TargetIcon, CalendarRange, CalendarDays, Calendar } from "lucide-react";
import type { Goal, Plan, Target, Task } from "@/api/types";
import { useDeleteGoal, useSaveGoal } from "@/api/plan";
import { areaMeta } from "@/lib/areas";
import { allowedParents, buildTree, filterTree, finer, HORIZONS, type GoalNode, type Horizon } from "@/lib/goals";
import { diffDays, fmtDay, isValidDay, type Day } from "@/lib/calendar/days";
import { AreaPicker } from "@/ui/AreaPicker";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, NativeSelect, Textarea } from "@/ui/Field";
import { Segmented } from "@/ui/Segmented";
import { EmptyState } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { areaVars } from "../calendar/EntryChip";
import { DeleteButton, errText, fieldErr } from "./shared";

export const HORIZON_META: Record<Horizon, { label: string; icon: typeof Mountain }> = {
  year: { label: "Year", icon: Mountain },
  quarter: { label: "Quarter", icon: CalendarRange },
  month: { label: "Month", icon: Calendar },
  week: { label: "Week", icon: CalendarDays },
};
const STATUS_TONE = { active: "accent", achieved: "good", paused: "warn", abandoned: "neutral" } as const;

export interface GoalEdit { goal?: Goal; parent?: Goal; horizon?: Horizon }

export function GoalsView({ goals, plans, targets, tasks, today, onEdit, highlight }: {
  goals: Goal[]; plans: Plan[]; targets: Target[]; tasks: Task[]; today: Day;
  onEdit: (e: GoalEdit) => void; highlight: string | null;
}) {
  const [show, setShow] = useState<"open" | "all">("open");
  const [collapsed, setCollapsed] = useState<Set<string>>(new Set());
  const counts = useMemo(() => {
    const m = new Map<string, { plans: number; targets: number; tasks: number }>();
    const bump = (id: string | null | undefined, k: "plans" | "targets" | "tasks") => {
      if (!id) return;
      const c = m.get(id) ?? { plans: 0, targets: 0, tasks: 0 };
      c[k]++;
      m.set(id, c);
    };
    plans.forEach((p) => bump(p.goal_id, "plans"));
    targets.forEach((t) => bump(t.goal_id, "targets"));
    tasks.forEach((t) => bump(t.goal_id, "tasks"));
    return m;
  }, [plans, targets, tasks]);
  const tree = useMemo(() => {
    const t = buildTree(goals);
    return show === "all" ? t : filterTree(t, (g) => g.status === "active" || g.status === "paused" || !g.status);
  }, [goals, show]);
  const doneCount = goals.filter((g) => g.status === "achieved").length;

  if (!goals.length) {
    return (
      <Card>
        <EmptyState
          art={<Flag className="size-10 text-accent" />}
          title="No goals yet"
          body="Start with what this year is for, then break it into quarters, months and weeks. The assistant uses them when it plans and when it adjusts scores."
          action={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => onEdit({ horizon: "year" })}>Add a year goal</Button>}
        />
      </Card>
    );
  }
  return (
    <div>
      <div className="mb-4 flex flex-wrap items-center gap-3">
        <Segmented
          label="Which goals"
          value={show}
          onChange={setShow}
          options={[{ value: "open", label: "Open" }, { value: "all", label: `All${doneCount ? ` · ${doneCount} achieved` : ""}` }]}
        />
        <div className="ml-auto hidden items-center gap-3 text-[12px] text-text-3 md:flex">
          {HORIZONS.map((h) => {
            const M = HORIZON_META[h];
            return <span key={h} className="flex items-center gap-1"><M.icon className="size-3.5" aria-hidden /> {M.label}</span>;
          })}
        </div>
      </div>
      <ul className="space-y-2" aria-label="Goals">
        {tree.map((n) => (
          <GoalRow key={n.goal.id} node={n} depth={0} today={today} counts={counts} collapsed={collapsed}
            toggle={(id) => setCollapsed((c) => { const x = new Set(c); if (x.has(id)) x.delete(id); else x.add(id); return x; })}
            onEdit={onEdit} highlight={highlight} />
        ))}
      </ul>
    </div>
  );
}

function GoalRow({ node, depth, today, counts, collapsed, toggle, onEdit, highlight }: {
  node: GoalNode; depth: number; today: Day; counts: Map<string, { plans: number; targets: number; tasks: number }>;
  collapsed: Set<string>; toggle: (id: string) => void; onEdit: (e: GoalEdit) => void; highlight: string | null;
}) {
  const g = node.goal;
  const H = HORIZON_META[g.horizon];
  const a = areaMeta(g.area);
  const c = counts.get(g.id);
  const open = !collapsed.has(g.id);
  const status = g.status ?? "active";
  const sub = finer(g.horizon);
  const due = g.due_on && isValidDay(g.due_on) ? diffDays(g.due_on, today) : null;
  const elapsed = g.starts_on && g.due_on && isValidDay(g.starts_on) && isValidDay(g.due_on)
    ? Math.max(0, Math.min(1, diffDays(today, g.starts_on) / Math.max(1, diffDays(g.due_on, g.starts_on))))
    : null;
  return (
    <li>
      <div
        id={`goal-${g.id}`}
        style={areaVars(g.area)}
        className={cn(
          "group relative flex items-center gap-3 rounded-2xl border bg-surface-1 py-3 pr-3 transition-colors hover:border-line-strong",
          status === "achieved" || status === "abandoned" ? "border-dashed border-line-strong bg-transparent" : "border-line",
          highlight === g.id && "border-accent/60 shadow-[0_0_0_1px_rgb(34_211_238/.3),0_0_24px_-6px_rgb(34_211_238/.5)]",
        )}
      >
        <span className="absolute inset-y-3 left-0 w-[3px] rounded-r-full bg-[var(--a)]" aria-hidden />
        <button
          onClick={() => node.children.length && toggle(g.id)}
          className={cn("ml-2 grid size-7 shrink-0 place-items-center rounded-md text-text-3", node.children.length ? "hover:bg-surface-2 hover:text-text" : "invisible")}
          aria-label={`Sub-goals of ${g.title}`}
          aria-expanded={node.children.length ? open : undefined}
          tabIndex={node.children.length ? 0 : -1}
        >
          <ChevronRight className={cn("size-4 transition-transform", open && "rotate-90")} />
        </button>
        <span className="grid size-9 shrink-0 place-items-center rounded-xl bg-[color-mix(in_oklab,var(--a)_14%,transparent)] text-[var(--a)]" title={H.label}>
          <H.icon className="size-4" aria-hidden />
        </span>
        <button className="min-w-0 flex-1 text-left" onClick={() => onEdit({ goal: g })}>
          <span className="flex items-center gap-2">
            <span className={cn("truncate text-[14.5px] font-medium text-text", status === "abandoned" && "line-through")}>{g.title}</span>
            {status !== "active" ? <Badge tone={STATUS_TONE[status]} className="capitalize">{status}</Badge> : null}
          </span>
          <span className="mt-0.5 flex flex-wrap items-center gap-x-3 gap-y-0.5 text-[12px] text-text-3">
            <span>{H.label}</span>
            <span className="flex items-center gap-1"><a.icon className="size-3 text-[var(--a)]" aria-hidden />{a.label}</span>
            {due != null ? (
              <span className={cn("flex items-center gap-1", status === "active" && due < 0 && "text-alert-text", status === "active" && due >= 0 && due <= 7 && "text-warn")}>
                <CalendarClock className="size-3" aria-hidden />
                {status === "active" ? (due < 0 ? `${-due}d overdue` : due === 0 ? "due today" : `${due}d left`) : `due ${fmtDay(g.due_on!, "d MMM")}`}
              </span>
            ) : null}
            {c?.plans ? <span className="flex items-center gap-1"><FileText className="size-3" aria-hidden />{c.plans} plan{c.plans > 1 ? "s" : ""}</span> : null}
            {c?.targets ? <span className="flex items-center gap-1"><TargetIcon className="size-3" aria-hidden />{c.targets} target{c.targets > 1 ? "s" : ""}</span> : null}
            {c?.tasks ? <span className="flex items-center gap-1"><ListTodo className="size-3" aria-hidden />{c.tasks} open task{c.tasks > 1 ? "s" : ""}</span> : null}
            {node.children.length ? <span>{node.children.length} sub-goal{node.children.length > 1 ? "s" : ""}</span> : null}
          </span>
          {elapsed != null && status === "active" ? (
            <span className="mt-2 block h-1 w-full max-w-64 overflow-hidden rounded-full bg-surface-3" title={`${Math.round(elapsed * 100)}% of the time gone`}>
              <span className="block h-full rounded-full bg-[var(--a)] opacity-70" style={{ width: `${elapsed * 100}%` }} />
            </span>
          ) : null}
        </button>
        {sub && status === "active" ? (
          <Button size="sm" variant="ghost" className="shrink-0 opacity-100 sm:opacity-0 sm:group-hover:opacity-100 sm:focus-visible:opacity-100" icon={<Plus className="size-3.5" />}
            onClick={() => onEdit({ parent: g, horizon: sub })} aria-label={`Add a ${HORIZON_META[sub].label.toLowerCase()} goal under ${g.title}`}>
            <span className="hidden md:inline">{HORIZON_META[sub].label}</span>
          </Button>
        ) : null}
      </div>
      {node.children.length && open ? (
        <ul aria-label={`Inside ${g.title}`} className="ml-[22px] mt-2 space-y-2 border-l border-line pl-4 sm:ml-[30px] sm:pl-5">
          {node.children.map((ch) => (
            <GoalRow key={ch.goal.id} node={ch} depth={depth + 1} today={today} counts={counts} collapsed={collapsed} toggle={toggle} onEdit={onEdit} highlight={highlight} />
          ))}
        </ul>
      ) : null}
    </li>
  );
}

export function GoalSheet({ edit, goals, onClose }: { edit: GoalEdit | null; goals: Goal[]; onClose: () => void }) {
  const g = edit?.goal;
  const [title, setTitle] = useState(g?.title ?? "");
  const [area, setArea] = useState(g?.area ?? edit?.parent?.area ?? null);
  const [horizon, setHorizon] = useState<Horizon>(g?.horizon ?? edit?.horizon ?? "month");
  const [parentId, setParentId] = useState(g?.parent_id ?? edit?.parent?.id ?? "");
  const [startsOn, setStartsOn] = useState(g?.starts_on ?? "");
  const [dueOn, setDueOn] = useState(g?.due_on ?? "");
  const [status, setStatus] = useState(g?.status ?? "active");
  const [description, setDescription] = useState(g?.description ?? "");
  const save = useSaveGoal();
  const del = useDeleteGoal();
  const parents = allowedParents(goals, horizon, g?.id);
  const parentOk = !parentId || parents.some((p) => p.id === parentId);
  const badDates = !!startsOn && !!dueOn && dueOn < startsOn;
  const valid = title.trim() && area && parentOk && !badDates;

  return (
    <Sheet
      open={!!edit}
      onOpenChange={(o) => !o && onClose()}
      title={g ? "Edit goal" : edit?.parent ? `New ${HORIZON_META[horizon].label.toLowerCase()} goal` : "New goal"}
      description={edit?.parent ? `Under “${edit.parent.title}”` : "Something you want to reach, over a year, quarter, month or week."}
      footer={
        <>
          {g ? <DeleteButton loading={del.isPending} onDelete={() => del.mutate(g, { onSuccess: () => { toast("Goal deleted"); onClose(); }, onError: (e) => toast.error(errText(e, "Couldn't delete")) })} /> : null}
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button type="submit" form="goal-form" variant="primary" disabled={!valid} loading={save.isPending}>{g ? "Save" : "Add goal"}</Button>
        </>
      }
    >
      <form
        id="goal-form"
        className="space-y-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (!valid) return;
          save.mutate(
            { existing: g, body: { title: title.trim(), area: area!, horizon, parent_id: parentId || null, starts_on: startsOn || null, due_on: dueOn || null, status, description: description.trim() || null } },
            { onSuccess: () => { toast.success(g ? "Goal saved" : "Goal added"); onClose(); }, onError: (err) => toast.error(errText(err)) },
          );
        }}
      >
        <Field label="Goal" error={fieldErr(save.error, "title")}>
          {(f) => <Input {...f} autoFocus required maxLength={200} value={title} onChange={(e) => setTitle(e.target.value)} placeholder="Ship the v2 release" />}
        </Field>
        <div>
          <p className="mb-2 text-[13px] font-medium text-text-2">Horizon</p>
          <Segmented<Horizon> label="Horizon" value={horizon} onChange={(h) => setHorizon(h)} options={HORIZONS.map((h) => ({ value: h, label: HORIZON_META[h].label }))} />
        </div>
        <AreaPicker value={area} onChange={setArea} />
        <Field label="Part of" hint={parents.length ? "Only longer horizons can hold this goal." : "A year goal sits at the top."} error={parentOk ? undefined : "That goal can't hold this horizon"}>
          {(f) => (
            <NativeSelect {...f} value={parentId} onChange={(e) => setParentId(e.target.value)} disabled={!parents.length}>
              <option value="">Nothing (top level)</option>
              {parents.map((p) => <option key={p.id} value={p.id}>{HORIZON_META[p.horizon].label}: {p.title}</option>)}
            </NativeSelect>
          )}
        </Field>
        <div className="grid grid-cols-2 gap-3">
          <Field label="Starts">{(f) => <Input {...f} type="date" value={startsOn} onChange={(e) => setStartsOn(e.target.value)} />}</Field>
          <Field label="Due" error={badDates ? "Due before it starts" : undefined}>{(f) => <Input {...f} type="date" value={dueOn} onChange={(e) => setDueOn(e.target.value)} />}</Field>
        </div>
        {g ? (
          <Field label="Status">
            {(f) => (
              <NativeSelect {...f} value={status} onChange={(e) => setStatus(e.target.value as typeof status)}>
                <option value="active">Active</option>
                <option value="paused">Paused</option>
                <option value="achieved">Achieved</option>
                <option value="abandoned">Abandoned</option>
              </NativeSelect>
            )}
          </Field>
        ) : null}
        <Field label="Why it matters" hint="Markdown. The assistant reads this when it plans and weighs your days.">
          {(f) => <Textarea {...f} rows={5} value={description} onChange={(e) => setDescription(e.target.value)} />}
        </Field>
      </form>
    </Sheet>
  );
}
