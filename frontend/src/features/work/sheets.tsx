import type { WorkDeliverable, WorkProject, WorkResponsibility, WorkRole } from "@/api/types";
import {
  useDeleteDeliverable, useDeleteProject, useDeleteResponsibility, useDeleteRole, useSaveDeliverable, useSaveProject,
  useSaveResponsibility, useSaveRole,
} from "@/api/work";
import { useState } from "react";
import { fromLocalInput, toLocalInput } from "@/lib/time";
import type { Day } from "@/lib/calendar/days";
import { RecurrenceBuilder } from "../common/RecurrenceBuilder";
import { FormSheet, opt } from "../common/FormSheet";

export const PROJECT_STATUS = [
  { value: "planned", label: "Planned" }, { value: "active", label: "Active" }, { value: "on_hold", label: "On hold" },
  { value: "done", label: "Done" }, { value: "cancelled", label: "Cancelled" },
];
export const DELIVERABLE_STATUS = [
  { value: "open", label: "Open" }, { value: "in_progress", label: "In progress" }, { value: "delivered", label: "Delivered" }, { value: "cancelled", label: "Cancelled" },
];

export function RoleSheet({ role, onClose }: { role?: WorkRole; onClose: () => void }) {
  const save = useSaveRole();
  const del = useDeleteRole();
  return (
    <FormSheet
      id="role-form"
      title={role ? "Edit role" : "New role"}
      description="A hat you wear at work: a job, a client, a side project."
      submitLabel={role ? "Save" : "Add role"}
      onClose={onClose}
      initial={{ title: role?.title ?? "", organization: role?.organization ?? "", description: role?.description ?? "", active: role?.active ?? true }}
      fields={[
        { key: "title", label: "Role", type: "text", required: true, maxLength: 200, placeholder: "Backend engineer" },
        { key: "organization", label: "Organisation", type: "text", maxLength: 200 },
        { key: "description", label: "Description", type: "textarea" },
        { key: "active", label: "Active", type: "checkbox", hint: "Inactive roles are kept for history." },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing: role, body: { title: String(v.title).trim(), organization: opt(v.organization), description: opt(v.description), active: !!v.active } })}
      onDelete={role ? () => del.mutateAsync(role) : undefined}
    />
  );
}

export function ResponsibilitySheet({ existing, roleId, roles, today, onClose }: { existing?: WorkResponsibility; roleId: string; roles: WorkRole[]; today: Day; onClose: () => void }) {
  const save = useSaveResponsibility();
  const del = useDeleteResponsibility();
  const [cadence, setCadence] = useState<string | null>(existing?.cadence ?? null);
  return (
    <FormSheet
      id="resp-form"
      title={existing ? "Edit responsibility" : "New responsibility"}
      description="Something you own in the role, done on a rhythm."
      submitLabel={existing ? "Save" : "Add"}
      onClose={onClose}
      initial={{ title: existing?.title ?? "", role_id: existing?.role_id ?? roleId, description: existing?.description ?? "", active: existing?.active ?? true }}
      fields={[
        { key: "title", label: "Responsibility", type: "text", required: true, maxLength: 200, placeholder: "Code review" },
        { key: "role_id", label: "Role", type: "select", options: roles.map((r) => ({ value: r.id, label: r.title })) },
        { key: "description", label: "Description", type: "textarea" },
        { key: "active", label: "Active", type: "checkbox" },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing, body: { title: String(v.title).trim(), role_id: String(v.role_id), cadence, description: opt(v.description), active: !!v.active } })}
      onDelete={existing ? () => del.mutateAsync(existing) : undefined}
    >
      <RecurrenceBuilder value={cadence} onChange={setCadence} startDay={today} today={today} />
    </FormSheet>
  );
}

export function ProjectSheet({ existing, roleId, roles, onClose, onDeleted }: { existing?: WorkProject; roleId?: string; roles: WorkRole[]; onClose: () => void; onDeleted?: () => void }) {
  const save = useSaveProject();
  const del = useDeleteProject();
  return (
    <FormSheet
      id="project-form"
      title={existing ? "Edit project" : "New project"}
      submitLabel={existing ? "Save" : "Add project"}
      onClose={onClose}
      initial={{ title: existing?.title ?? "", role_id: existing?.role_id ?? roleId ?? "", status: existing?.status ?? "active", starts_on: existing?.starts_on ?? "", due_on: existing?.due_on ?? "", description: existing?.description ?? "" }}
      validate={(v): Record<string, string> => (v.starts_on && v.due_on && String(v.due_on) < String(v.starts_on) ? { due_on: "Due before it starts" } : {})}
      fields={[
        { key: "title", label: "Project", type: "text", required: true, maxLength: 200, placeholder: "Payments v2" },
        { key: "role_id", label: "Role", type: "select", options: [{ value: "", label: "No role" }, ...roles.map((r) => ({ value: r.id, label: r.title }))] },
        { key: "status", label: "Status", type: "select", options: PROJECT_STATUS },
        { key: "starts_on", label: "Starts", type: "date" },
        { key: "due_on", label: "Due", type: "date" },
        { key: "description", label: "Description", type: "textarea" },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing, body: { title: String(v.title).trim(), role_id: opt(v.role_id), status: String(v.status) as WorkProject["status"], starts_on: opt(v.starts_on), due_on: opt(v.due_on), description: opt(v.description) } })}
      onDelete={existing ? () => del.mutateAsync(existing).then(() => onDeleted?.()) : undefined}
    />
  );
}

export function DeliverableSheet({ existing, projectId, tz, onClose }: { existing?: WorkDeliverable; projectId: string; tz: string; onClose: () => void }) {
  const save = useSaveDeliverable();
  const del = useDeleteDeliverable();
  return (
    <FormSheet
      id="deliverable-form"
      title={existing ? "Edit deliverable" : "New deliverable"}
      description="Something the project hands over, by a date."
      submitLabel={existing ? "Save" : "Add"}
      onClose={onClose}
      initial={{ title: existing?.title ?? "", due_at: existing?.due_at ? toLocalInput(existing.due_at, tz) : "", status: existing?.status ?? "open", description: existing?.description ?? "" }}
      fields={[
        { key: "title", label: "Deliverable", type: "text", required: true, maxLength: 200, placeholder: "Design doc signed off" },
        { key: "due_at", label: "Due", type: "datetime" },
        { key: "status", label: "Status", type: "select", options: DELIVERABLE_STATUS },
        { key: "description", label: "Description", type: "textarea" },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing, body: { project_id: projectId, title: String(v.title).trim(), due_at: v.due_at ? fromLocalInput(String(v.due_at), tz) : null, status: String(v.status) as WorkDeliverable["status"], description: opt(v.description) } })}
      onDelete={existing ? () => del.mutateAsync(existing) : undefined}
    />
  );
}
