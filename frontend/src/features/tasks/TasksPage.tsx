import { useMemo, useState } from "react";
import { useSearchParams } from "react-router";
import { Check, CheckCircle2, CircleDashed, Clock, ListTodo, Plus, Repeat, Search } from "lucide-react";
import { useUser } from "@/api/auth";
import { useGoals } from "@/api/plan";
import { useSaveTask, useSetTaskStatus, useTaskList } from "@/api/tasks";
import { useProjects } from "@/api/work";
import { AREAS, type Area, type Task } from "@/api/types";
import { areaMeta } from "@/lib/areas";
import { describeRule } from "@/lib/rrule";
import { GROUPS, groupOf, localDayEndMs, PRIORITY, sortTasks, type Group } from "@/lib/tasks";
import { fmt, localDay, relative } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Input, NativeSelect } from "@/ui/Field";
import { Chip, Segmented } from "@/ui/Segmented";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { areaVars } from "../calendar/EntryChip";
import { OriginBadge } from "../common/OriginBadge";
import { Page } from "../common/Page";
import { TaskSheet } from "./TaskSheet";

const EMPTY: Record<Group, string> = {
  today: "Nothing due today.",
  overdue: "Nothing overdue. Nice.",
  upcoming: "Nothing scheduled ahead.",
  recurring: "No repeating tasks. Make one by setting Repeat on a task.",
  nodate: "Every task has a date.",
  done: "Completed tasks show up here.",
};

export function TasksPage() {
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const today = localDay(now, tz, user.day_start_hour);
  const [params, setParams] = useSearchParams();
  const group = (params.get("g") as Group | null) ?? "today";
  const open = useTaskList("open");
  const doneQ = useTaskList("done");
  const goals = useGoals();
  const projects = useProjects();
  const [areas, setAreas] = useState<Set<Area>>(new Set());
  const [minPriority, setMinPriority] = useState("0");
  const [goalId, setGoalId] = useState("");
  const [projectId, setProjectId] = useState("");
  const [q, setQ] = useState("");
  const [editing, setEditing] = useState<Task | "new" | null>(null);
  // ?task=<id> (from search or a link) opens that task's sheet once it's loaded.
  const linked = params.get("task");
  const [openedLink, setOpenedLink] = useState<string | null>(null);
  if (linked && linked !== openedLink) {
    const t = [...(open.data ?? []), ...(doneQ.data ?? [])].find((x) => x.id === linked);
    // Settle once found, or once both lists are in (either may arrive first).
    if (t || (open.data && doneQ.data)) {
      setOpenedLink(linked);
      if (t) setEditing(t);
    }
  }
  const setStatus = useSetTaskStatus();
  const quick = useSaveTask();
  const [quickTitle, setQuickTitle] = useState("");

  const all = useMemo(() => [...(open.data ?? []), ...(doneQ.data ?? [])], [open.data, doneQ.data]);
  const filtered = useMemo(() => all.filter((t) =>
    (!areas.size || areas.has(t.area)) && (t.priority ?? 1) >= Number(minPriority) &&
    (!goalId || t.goal_id === goalId) && (!projectId || t.project_id === projectId) &&
    (!q.trim() || t.title.toLowerCase().includes(q.trim().toLowerCase()))), [all, areas, minPriority, goalId, projectId, q]);
  const byGroup = useMemo(() => {
    const m = new Map<Group, Task[]>(GROUPS.map((g) => [g.id, []]));
    for (const t of filtered) m.get(groupOf(t, now, today, tz, user.day_start_hour))!.push(t);
    return m;
  }, [filtered, now, today, tz, user.day_start_hour]);
  const list = sortTasks(byGroup.get(group) ?? [], group);
  const pending = open.isPending || (group === "done" && doneQ.isPending);

  const addQuick = () => {
    const title = quickTitle.trim();
    if (!title) return;
    // Quick add lands in the group being looked at: today's gets due at the end of the day.
    const due = group === "today" ? new Date(localDayEndMs(today, tz, user.day_start_hour) - 60_000).toISOString() : null;
    quick.mutate(
      { body: { title, area: areas.size === 1 ? [...areas][0]! : "personal", due_at: due, goal_id: goalId || null, project_id: projectId || null } },
      { onSuccess: () => { setQuickTitle(""); toast.success("Task added"); }, onError: () => toast.error("Couldn't add the task") },
    );
  };

  return (
    <Page
      wide
      title="Tasks"
      subtitle="One-off and repeating things to do. The assistant plans around them."
      actions={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setEditing("new")}>New task</Button>}
    >
      <Segmented<Group>
        className="mb-4 max-w-full overflow-x-auto"
        label="Task groups"
        value={group}
        onChange={(g) => setParams(g === "today" ? {} : { g })}
        options={GROUPS.map((g) => {
          const n = byGroup.get(g.id)?.length ?? 0;
          return { value: g.id, label: <span className="flex items-center gap-1.5">{g.label}{n && g.id !== "done" ? <span className={cn("rounded-full px-1.5 text-[11px]", g.id === "overdue" ? "bg-alert/20 text-alert-text" : "bg-surface-3 text-text-2")}>{n}</span> : null}</span> };
        })}
      />
      <div className="mb-4 flex flex-wrap items-center gap-2" role="group" aria-label="Filters">
        <div className="relative">
          <Search className="pointer-events-none absolute left-2.5 top-1/2 size-4 -translate-y-1/2 text-text-3" aria-hidden />
          <Input aria-label="Search tasks" className="h-8 w-48 pl-8 text-[13px]" value={q} onChange={(e) => setQ(e.target.value)} placeholder="Search" />
        </div>
        {AREAS.map((a) => {
          const m = areaMeta(a);
          return (
            <Chip key={a} color={m.color} on={areas.has(a)} onClick={() => setAreas((s) => { const x = new Set(s); if (x.has(a)) x.delete(a); else x.add(a); return x; })}>
              <m.icon className="size-3.5" style={{ color: m.color }} aria-hidden /> {m.label}
            </Chip>
          );
        })}
        <NativeSelect aria-label="Priority" className="h-8 w-auto text-[13px]" value={minPriority} onChange={(e) => setMinPriority(e.target.value)}>
          <option value="0">Any priority</option>
          <option value="2">High and up</option>
          <option value="3">Critical</option>
        </NativeSelect>
        <NativeSelect aria-label="Goal" className="h-8 w-auto max-w-48 text-[13px]" value={goalId} onChange={(e) => setGoalId(e.target.value)}>
          <option value="">Any goal</option>
          {(goals.data ?? []).map((g) => <option key={g.id} value={g.id}>{g.title}</option>)}
        </NativeSelect>
        <NativeSelect aria-label="Project" className="h-8 w-auto max-w-48 text-[13px]" value={projectId} onChange={(e) => setProjectId(e.target.value)}>
          <option value="">Any project</option>
          {(projects.data ?? []).map((p) => <option key={p.id} value={p.id}>{p.title}</option>)}
        </NativeSelect>
      </div>

      <Card className="p-2">
        {group !== "done" && group !== "recurring" ? (
          <form className="flex items-center gap-2 border-b border-line px-3 pb-2 pt-1" onSubmit={(e) => { e.preventDefault(); addQuick(); }}>
            <Plus className="size-4 text-accent" aria-hidden />
            <input
              aria-label="Quick add a task"
              className="h-9 flex-1 bg-transparent text-sm text-text outline-none placeholder:text-text-3"
              placeholder={group === "today" ? "Add a task for today, press Enter" : "Add a task, press Enter"}
              value={quickTitle}
              onChange={(e) => setQuickTitle(e.target.value)}
            />
            {quickTitle ? <Button size="sm" type="submit" loading={quick.isPending}>Add</Button> : null}
          </form>
        ) : null}
        {pending ? (
          <div className="space-y-2 p-2">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-12" />)}</div>
        ) : open.error ? (
          <ErrorState error={open.error} retry={() => open.refetch()} />
        ) : !list.length ? (
          <EmptyState className="py-10" art={<ListTodo className="size-9 text-accent" />} title={EMPTY[group]} />
        ) : (
          <ul className="divide-y divide-line" aria-label={`${GROUPS.find((g) => g.id === group)!.label} tasks`}>
            {list.map((t) => (
              <TaskRow key={t.id} t={t} tz={tz} now={now} onOpen={() => setEditing(t)}
                onToggle={() => setStatus.mutate({ id: t.id, status: t.status === "done" ? "open" : "done" }, {
                  onSuccess: () => toast(t.status === "done" ? `Reopened: ${t.title}` : `Done: ${t.title}`),
                  onError: () => toast.error("Couldn't update the task"),
                })} />
            ))}
          </ul>
        )}
      </Card>
      {editing ? (
        <TaskSheet key={editing === "new" ? "new" : editing.id} task={editing === "new" ? null : editing} open onClose={() => setEditing(null)} tz={tz} today={today}
          defaults={{ goal_id: goalId || undefined, project_id: projectId || undefined }} />
      ) : null}
    </Page>
  );
}

function TaskRow({ t, tz, now, onOpen, onToggle }: { t: Task; tz: string; now: number; onOpen: () => void; onToggle: () => void }) {
  const a = areaMeta(t.area);
  const done = t.status === "done";
  const cancelled = t.status === "cancelled";
  const overdue = !done && !t.rrule && t.due_at && Date.parse(t.due_at) < now;
  const pr = PRIORITY[t.priority ?? 1]!;
  const rule = t.rrule ? describeRule(t.rrule) : null;
  return (
    <li style={areaVars(t.area)} className="group flex items-center gap-3 rounded-xl px-3 py-2.5 hover:bg-surface-2/60">
      {t.rrule ? (
        <span className="grid size-6 shrink-0 place-items-center text-text-3" title="Repeating: complete each day from its occurrences"><Repeat className="size-4" aria-hidden /></span>
      ) : (
        <button
          onClick={onToggle}
          aria-label={done ? `Reopen ${t.title}` : `Mark ${t.title} done`}
          className={cn("grid size-6 shrink-0 place-items-center rounded-full border-2 transition-colors",
            done ? "border-good bg-good text-black" : "border-[color-mix(in_oklab,var(--a)_70%,transparent)] hover:bg-[color-mix(in_oklab,var(--a)_18%,transparent)]")}
        >
          {done ? <Check className="size-3.5" strokeWidth={3} /> : null}
        </button>
      )}
      <button onClick={onOpen} className="min-w-0 flex-1 text-left">
        <span className={cn("flex items-center gap-2 text-sm", done || cancelled ? "text-text-3 line-through" : "text-text")}>
          {pr.marks ? <span className={cn("font-mono text-[12px] font-bold", t.priority === 3 ? "text-alert-text" : "text-warn")} aria-label={`${pr.label} priority`}>{pr.marks}</span> : null}
          <span className="truncate">{t.title}</span>
        </span>
        <span className="mt-0.5 flex flex-wrap items-center gap-x-3 text-[12px] text-text-3">
          <span className="flex items-center gap-1"><a.icon className="size-3 text-[var(--a)]" aria-hidden />{a.label}</span>
          {t.due_at && !t.rrule ? (
            <span className={cn("flex items-center gap-1", overdue && "text-alert-text")} title={fmt(t.due_at, tz, "EEE d MMM, HH:mm")}>
              <Clock className="size-3" aria-hidden />{overdue ? "overdue · " : ""}{relative(t.due_at, now)}
            </span>
          ) : null}
          {rule ? <span className="flex items-center gap-1 first-letter:uppercase"><Repeat className="size-3" aria-hidden />{rule}</span> : null}
          {t.estimate_min ? <span>{t.estimate_min} min</span> : null}
          {done && t.completed_at ? <span className="flex items-center gap-1"><CheckCircle2 className="size-3 text-good" aria-hidden />{relative(t.completed_at, now)}</span> : null}
          {cancelled ? <span className="flex items-center gap-1"><CircleDashed className="size-3" aria-hidden />cancelled</span> : null}
        </span>
      </button>
      <OriginBadge row={t} />
    </li>
  );
}
