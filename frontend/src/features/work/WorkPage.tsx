import { useState } from "react";
import { Link } from "react-router";
import { Briefcase, Building2, CalendarClock, ChevronRight, FolderKanban, Plus, RefreshCw, Settings2 } from "lucide-react";
import { useUser } from "@/api/auth";
import { useDeliverables, useProjects, useResponsibilities, useRoles, useWorkContexts } from "@/api/work";
import type { WorkProject, WorkResponsibility, WorkRole } from "@/api/types";
import { diffDays, fmtDay, isValidDay } from "@/lib/calendar/days";
import { localDay } from "@/lib/time";
import { describeRule } from "@/lib/rrule";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { cn } from "@/ui/cn";
import { OriginBadge } from "../common/OriginBadge";
import { Page } from "../common/Page";
import { PROJECT_STATUS, ProjectSheet, ResponsibilitySheet, RoleSheet } from "./sheets";

const PROJECT_TONE = { planned: "neutral", active: "accent", on_hold: "warn", done: "good", cancelled: "neutral" } as const;

export function WorkPage() {
  const user = useUser();
  const today = localDay(useNow(), user.timezone, user.day_start_hour);
  const roles = useRoles();
  const resp = useResponsibilities();
  const projects = useProjects();
  const deliverables = useDeliverables();
  const contexts = useWorkContexts();
  const [roleEdit, setRoleEdit] = useState<WorkRole | "new" | null>(null);
  const [respEdit, setRespEdit] = useState<{ existing?: WorkResponsibility; roleId: string } | null>(null);
  const [projectNew, setProjectNew] = useState<{ roleId?: string } | null>(null);
  const roleList = [...(roles.data ?? [])].sort((a, b) => Number(b.active ?? true) - Number(a.active ?? true) || a.title.localeCompare(b.title));
  const openDeliverables = (deliverables.data ?? []).filter((d) => d.status === "open" || d.status === "in_progress");
  const loose = (projects.data ?? []).filter((p) => !p.role_id || !roleList.some((r) => r.id === p.role_id));
  const ctx = (contexts.data ?? []).filter((c) => !c.active_to || c.active_to >= today);

  return (
    <Page
      wide
      title="Work"
      subtitle="Roles, what you're responsible for, and the projects that ship."
      actions={
        <>
          <Button icon={<FolderKanban className="size-4" />} onClick={() => setProjectNew({})}>New project</Button>
          <Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setRoleEdit("new")}>New role</Button>
        </>
      }
    >
      <Card className="mb-5 flex flex-wrap items-center gap-4 p-4">
        <span className="grid size-9 place-items-center rounded-xl bg-[color-mix(in_oklab,var(--color-area-work)_15%,transparent)] text-[var(--color-area-work)]"><Settings2 className="size-4" aria-hidden /></span>
        <div className="min-w-0 flex-1">
          <p className="text-sm font-medium text-text">Work tracking</p>
          <p className="text-[12.5px] text-text-3">
            {ctx.length ? ctx.map((c) => `${c.name}: ${c.evidence_mode === "observed" ? "seen by the collector" : "self-reported"}, ${Math.round((c.workday_target_min ?? 0) / 60 * 10) / 10} h a workday`).join(" · ") : "How work time is measured (observed or self-reported) and the workday target."}
          </p>
        </div>
        <Link to="/settings/tracking" className="flex items-center gap-1 text-[13px] text-accent hover:underline">Set up <ChevronRight className="size-3.5" /></Link>
      </Card>

      {roles.isPending ? (
        <div className="grid gap-4 lg:grid-cols-2">{[0, 1].map((i) => <Skeleton key={i} className="h-56" />)}</div>
      ) : roles.error ? (
        <ErrorState error={roles.error} retry={() => roles.refetch()} />
      ) : !roleList.length && !loose.length ? (
        <Card>
          <EmptyState art={<Briefcase className="size-10 text-accent" />} title="No roles yet"
            body="Add the role you work in, then the things you're responsible for and the projects you're shipping. The assistant uses them to plan your workday."
            action={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setRoleEdit("new")}>Add a role</Button>} />
        </Card>
      ) : (
        <div className="grid gap-5 lg:grid-cols-2">
          {roleList.map((r) => {
            const rs = (resp.data ?? []).filter((x) => x.role_id === r.id);
            const ps = (projects.data ?? []).filter((p) => p.role_id === r.id);
            return (
              <Card key={r.id} className={cn("p-5", r.active === false && "border-dashed bg-transparent")} aria-label={r.title}>
                <div className="flex items-start gap-3">
                  <span className="grid size-10 shrink-0 place-items-center rounded-xl bg-[color-mix(in_oklab,var(--color-area-work)_15%,transparent)] text-[var(--color-area-work)]"><Briefcase className="size-5" aria-hidden /></span>
                  <div className="min-w-0 flex-1">
                    <button onClick={() => setRoleEdit(r)} className="text-left text-[16px] font-semibold text-text hover:text-accent">{r.title}</button>
                    {r.organization ? <p className="flex items-center gap-1 text-[12.5px] text-text-2"><Building2 className="size-3.5" aria-hidden />{r.organization}</p> : null}
                  </div>
                  {r.active === false ? <Badge>Inactive</Badge> : null}
                  <OriginBadge row={r} />
                </div>
                <div className="mt-4">
                  <div className="mb-1.5 flex items-center justify-between">
                    <p className="text-[11px] font-semibold uppercase tracking-wider text-text-3">Responsibilities</p>
                    <button className="text-[12.5px] text-accent hover:underline" onClick={() => setRespEdit({ roleId: r.id })} aria-label={`Add a responsibility to ${r.title}`}>+ Add</button>
                  </div>
                  {rs.length ? (
                    <ul className="space-y-1">
                      {rs.map((x) => (
                        <li key={x.id}>
                          <button onClick={() => setRespEdit({ existing: x, roleId: r.id })} className={cn("flex w-full items-center gap-2 rounded-lg px-2 py-1.5 text-left text-[13.5px] hover:bg-surface-2", x.active === false ? "text-text-3 line-through" : "text-text")}>
                            <RefreshCw className="size-3.5 shrink-0 text-text-3" aria-hidden />
                            <span className="min-w-0 flex-1 truncate">{x.title}</span>
                            {x.cadence ? <span className="text-[12px] text-text-3 first-letter:uppercase">{describeRule(x.cadence) ?? x.cadence}</span> : null}
                          </button>
                        </li>
                      ))}
                    </ul>
                  ) : <p className="px-2 text-[12.5px] text-text-3">None yet.</p>}
                </div>
                <div className="mt-4">
                  <div className="mb-1.5 flex items-center justify-between">
                    <p className="text-[11px] font-semibold uppercase tracking-wider text-text-3">Projects</p>
                    <button className="text-[12.5px] text-accent hover:underline" onClick={() => setProjectNew({ roleId: r.id })} aria-label={`Add a project to ${r.title}`}>+ Add</button>
                  </div>
                  {ps.length ? <ProjectList projects={ps} today={today} openDeliverables={openDeliverables} /> : <p className="px-2 text-[12.5px] text-text-3">None yet.</p>}
                </div>
              </Card>
            );
          })}
          {loose.length ? (
            <Card className="p-5" aria-label="Projects without a role">
              <p className="mb-2 text-[11px] font-semibold uppercase tracking-wider text-text-3">Projects without a role</p>
              <ProjectList projects={loose} today={today} openDeliverables={openDeliverables} />
            </Card>
          ) : null}
        </div>
      )}
      {roleEdit ? <RoleSheet role={roleEdit === "new" ? undefined : roleEdit} onClose={() => setRoleEdit(null)} /> : null}
      {respEdit ? <ResponsibilitySheet existing={respEdit.existing} roleId={respEdit.roleId} roles={roleList} today={today} onClose={() => setRespEdit(null)} /> : null}
      {projectNew ? <ProjectSheet roleId={projectNew.roleId} roles={roleList} onClose={() => setProjectNew(null)} /> : null}
    </Page>
  );
}

function ProjectList({ projects, today, openDeliverables }: { projects: WorkProject[]; today: string; openDeliverables: { project_id: string; due_at?: string | null }[] }) {
  return (
    <ul className="space-y-1">
      {projects.map((p) => {
        const open = openDeliverables.filter((d) => d.project_id === p.id).length;
        const due = p.due_on && isValidDay(p.due_on) ? diffDays(p.due_on, today) : null;
        const live = p.status === "active" || p.status === "planned" || !p.status;
        return (
          <li key={p.id}>
            <Link to={`/work/projects/${p.id}`} className="flex items-center gap-2 rounded-lg px-2 py-2 text-[13.5px] hover:bg-surface-2">
              <FolderKanban className="size-4 shrink-0 text-[var(--color-area-work)]" aria-hidden />
              <span className={cn("min-w-0 flex-1 truncate", live ? "text-text" : "text-text-3")}>{p.title}</span>
              {open ? <span className="text-[12px] text-text-3">{open} open</span> : null}
              {due != null && live ? (
                <span className={cn("flex items-center gap-1 text-[12px]", due < 0 ? "text-alert-text" : due <= 7 ? "text-warn" : "text-text-3")} title={fmtDay(p.due_on!, "d MMM yyyy")}>
                  <CalendarClock className="size-3" aria-hidden />{due < 0 ? `${-due}d late` : `${due}d`}
                </span>
              ) : null}
              <Badge tone={PROJECT_TONE[p.status ?? "active"]}>{PROJECT_STATUS.find((s) => s.value === (p.status ?? "active"))!.label}</Badge>
            </Link>
          </li>
        );
      })}
    </ul>
  );
}
