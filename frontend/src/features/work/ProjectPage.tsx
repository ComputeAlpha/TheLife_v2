import { useState } from "react";
import { Link, useNavigate, useParams } from "react-router";
import { ArrowLeft, CheckCircle2, CircleDot, ListTodo, PackageCheck, PencilLine, Plus } from "lucide-react";
import { useUser } from "@/api/auth";
import { useTaskList } from "@/api/tasks";
import { useDeliverables, useProjects, useRoles } from "@/api/work";
import type { Task, WorkDeliverable } from "@/api/types";
import { fmt, localDay, relative } from "@/lib/time";
import { fmtDay } from "@/lib/calendar/days";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { cn } from "@/ui/cn";
import { Page } from "../common/Page";
import { TaskSheet } from "../tasks/TaskSheet";
import { DELIVERABLE_STATUS, DeliverableSheet, PROJECT_STATUS, ProjectSheet } from "./sheets";

export function ProjectPage() {
  const { id } = useParams();
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const today = localDay(now, tz, user.day_start_hour);
  const navigate = useNavigate();
  const projects = useProjects();
  const roles = useRoles();
  const deliverables = useDeliverables();
  const tasks = useTaskList("open");
  const [edit, setEdit] = useState(false);
  const [del, setDel] = useState<WorkDeliverable | "new" | null>(null);
  const [task, setTask] = useState<Task | "new" | null>(null);
  const p = projects.data?.find((x) => x.id === id);
  if (projects.isPending) return <Page title="…"><Skeleton className="h-64" /></Page>;
  if (projects.error) return <Page title="Work"><ErrorState error={projects.error} retry={() => projects.refetch()} /></Page>;
  if (!p) return <Page title="Not found"><Card><EmptyState title="No such project" action={<Button onClick={() => navigate("/work")}>Back to Work</Button>} /></Card></Page>;
  const role = roles.data?.find((r) => r.id === p.role_id);
  const ds = (deliverables.data ?? []).filter((d) => d.project_id === p.id).sort((a, b) => (a.due_at ?? "9999").localeCompare(b.due_at ?? "9999"));
  const linked = (tasks.data ?? []).filter((t) => t.project_id === p.id);
  const nowIdx = ds.findIndex((d) => d.due_at && Date.parse(d.due_at) > now);

  return (
    <Page
      wide
      title={<span className="flex items-center gap-3"><Link to="/work" aria-label="Back to Work" className="rounded-lg p-1 text-text-3 hover:bg-surface-2 hover:text-text"><ArrowLeft className="size-5" /></Link>{p.title}</span>}
      subtitle={<span className="flex flex-wrap items-center gap-2">{role ? role.title : "No role"}{p.starts_on && p.due_on ? ` · ${fmtDay(p.starts_on, "d MMM")} – ${fmtDay(p.due_on, "d MMM yyyy")}` : p.due_on ? ` · due ${fmtDay(p.due_on, "d MMM yyyy")}` : p.starts_on ? ` · from ${fmtDay(p.starts_on, "d MMM yyyy")}` : ""}<Badge tone="accent">{PROJECT_STATUS.find((s) => s.value === (p.status ?? "active"))!.label}</Badge></span>}
      actions={<Button icon={<PencilLine className="size-4" />} onClick={() => setEdit(true)}>Edit</Button>}
    >
      {p.description ? <p className="mb-5 max-w-3xl text-sm text-text-2">{p.description}</p> : null}
      <div className="grid gap-5 lg:grid-cols-[minmax(0,7fr)_minmax(0,5fr)]">
        <Card>
          <CardHeader title="Deliverables" icon={<PackageCheck className="size-4" />} subtitle="By due date" action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setDel("new")}>Add</Button>} />
          {!ds.length ? (
            <EmptyState className="py-10" title="No deliverables yet" body="What does this project hand over, and by when?" />
          ) : (
            <ol className="relative mx-5 mb-5 border-l border-line pl-6" aria-label="Deliverables">
              {ds.map((d, i) => {
                const doneish = d.status === "delivered" || d.status === "cancelled";
                const late = !doneish && d.due_at && Date.parse(d.due_at) < now;
                return (
                  <li key={d.id} className="relative pb-5 last:pb-0">
                    {i === nowIdx ? <span className="absolute -left-[31px] -top-3 flex items-center gap-1 text-[10.5px] font-semibold uppercase tracking-wider text-accent"><span className="h-px w-3 bg-accent" />Today</span> : null}
                    <span className={cn("absolute -left-[31px] top-1 grid size-3.5 place-items-center rounded-full border-2 bg-surface-1",
                      d.status === "delivered" ? "border-good bg-good" : late ? "border-alert-text" : d.status === "in_progress" ? "border-accent" : "border-line-strong")} aria-hidden />
                    <button onClick={() => setDel(d)} className="w-full rounded-xl border border-line p-3 text-left transition-colors hover:border-line-strong">
                      <span className="flex items-center gap-2">
                        {d.status === "delivered" ? <CheckCircle2 className="size-4 text-good" aria-hidden /> : <CircleDot className={cn("size-4", d.status === "in_progress" ? "text-accent" : "text-text-3")} aria-hidden />}
                        <span className={cn("min-w-0 flex-1 truncate text-sm font-medium", doneish ? "text-text-3 line-through" : "text-text")}>{d.title}</span>
                        <Badge tone={d.status === "delivered" ? "good" : d.status === "in_progress" ? "accent" : late ? "alert" : "neutral"}>{late ? "Late" : DELIVERABLE_STATUS.find((s) => s.value === (d.status ?? "open"))!.label}</Badge>
                      </span>
                      {d.due_at ? <span className={cn("mt-1 block text-[12.5px]", late ? "text-alert-text" : "text-text-3")}>{fmt(d.due_at, tz, "EEE d MMM, HH:mm")} · {relative(d.due_at, now)}</span> : <span className="mt-1 block text-[12.5px] text-text-3">No due date</span>}
                    </button>
                  </li>
                );
              })}
            </ol>
          )}
        </Card>
        <Card>
          <CardHeader title="Tasks" icon={<ListTodo className="size-4" />} subtitle="Open tasks in this project" action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setTask("new")}>Add</Button>} />
          {!linked.length ? <EmptyState className="py-8" title="No open tasks" /> : (
            <ul className="divide-y divide-line px-2 pb-2">
              {linked.map((t) => (
                <li key={t.id}>
                  <button onClick={() => setTask(t)} className="flex w-full items-center gap-3 rounded-xl px-3 py-2.5 text-left hover:bg-surface-2">
                    <span className="min-w-0 flex-1 truncate text-sm text-text">{t.title}</span>
                    {t.due_at ? <span className="text-[12px] text-text-3">{relative(t.due_at, now)}</span> : null}
                  </button>
                </li>
              ))}
            </ul>
          )}
        </Card>
      </div>
      {edit ? <ProjectSheet existing={p} roles={roles.data ?? []} onClose={() => setEdit(false)} onDeleted={() => navigate("/work")} /> : null}
      {del ? <DeliverableSheet existing={del === "new" ? undefined : del} projectId={p.id} tz={tz} onClose={() => setDel(null)} /> : null}
      {task ? <TaskSheet key={task === "new" ? "new" : task.id} task={task === "new" ? null : task} open onClose={() => setTask(null)} tz={tz} today={today} defaults={{ project_id: p.id }} /> : null}
    </Page>
  );
}
