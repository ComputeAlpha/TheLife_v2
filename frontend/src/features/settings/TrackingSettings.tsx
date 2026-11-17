import { useState } from "react";
import { Briefcase, Plus, Wand2 } from "lucide-react";
import { useDeleteIngestRule, useIngestRules, useSaveIngestRule, useDeviceList } from "@/api/settings";
import { useDeleteWorkContext, useSaveWorkContext, useWorkContexts } from "@/api/work";
import type { IngestRule, WorkContext } from "@/api/types";
import { areaMeta } from "@/lib/areas";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { FormSheet, opt } from "../common/FormSheet";
import { Switch } from "@/ui/Switch";
import { errText } from "../plan/shared";

export function TrackingSettings() {
  return (
    <div className="space-y-6">
      <RulesCard />
      <ContextsCard />
    </div>
  );
}

function RulesCard() {
  const q = useIngestRules();
  const save = useSaveIngestRule();
  const [edit, setEdit] = useState<IngestRule | "new" | null>(null);
  const list = q.data ?? [];
  return (
    <Card>
      <CardHeader title="App rules" icon={<Wand2 className="size-4" />} subtitle="Turn a window or program straight into a detection, without the vision model." action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setEdit("new")}>New rule</Button>} />
      <div className="px-5 pb-5">
        {q.isPending ? <Skeleton className="h-16" /> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : !list.length ? (
          <EmptyState className="py-6" title="No rules" body="For example: window class “code” → app VS Code, category coding, area Work." />
        ) : (
          <ul className="divide-y divide-line" aria-label="App rules">
            {list.map((r) => {
              const a = areaMeta(r.area);
              const on = r.enabled ?? true;
              const match = [r.match_wm_class && `class “${r.match_wm_class}”`, r.match_exe && `program “${r.match_exe}”`, r.match_title && `title ~ “${r.match_title}”`].filter(Boolean).join(" and ");
              return (
                <li key={r.id} className="flex items-center gap-3 py-3">
                  <button onClick={() => setEdit(r)} className="min-w-0 flex-1 text-left">
                    <span className={cn("block truncate text-sm", on ? "text-text" : "text-text-3")}>{match || "Matches nothing"} <span className="text-text-3">→</span> {r.app}</span>
                    <span className="flex items-center gap-2 text-[12px] text-text-3">
                      <a.icon className="size-3" style={{ color: a.color }} aria-hidden />{a.label} · {r.category} · {Math.round((r.confidence ?? 0.9) * 100)}% sure
                    </span>
                  </button>
                  <Switch on={on} label={`Rule for ${r.app} enabled`} onChange={(v) => save.mutate({ existing: r, body: { enabled: v } }, { onError: (e) => toast.error(errText(e)) })} />
                </li>
              );
            })}
          </ul>
        )}
      </div>
      {edit ? <RuleSheet existing={edit === "new" ? undefined : edit} onClose={() => setEdit(null)} /> : null}
    </Card>
  );
}

function RuleSheet({ existing, onClose }: { existing?: IngestRule; onClose: () => void }) {
  const save = useSaveIngestRule();
  const del = useDeleteIngestRule();
  const r = existing;
  return (
    <FormSheet
      id="rule-form"
      title={r ? "Edit app rule" : "New app rule"}
      description="Any field you fill must match. Title is a case-insensitive substring; class and program match exactly."
      submitLabel={r ? "Save" : "Add rule"}
      onClose={onClose}
      initial={{ match_wm_class: r?.match_wm_class ?? "", match_exe: r?.match_exe ?? "", match_title: r?.match_title ?? "", app: r?.app ?? "", category: r?.category ?? "", area: r?.area ?? "", confidence: String(r?.confidence ?? 0.9), enabled: r?.enabled ?? true }}
      validate={(v): Record<string, string> => {
        const e: Record<string, string> = {};
        if (!opt(v.match_wm_class) && !opt(v.match_exe) && !opt(v.match_title)) e.match_title = "Fill at least one match field";
        const c = Number(v.confidence);
        if (!(c >= 0 && c <= 1)) e.confidence = "Between 0 and 1";
        return e;
      }}
      fields={[
        { key: "match_wm_class", label: "Window class", type: "text", placeholder: "code", maxLength: 200 },
        { key: "match_exe", label: "Program", type: "text", placeholder: "/usr/bin/code", maxLength: 200 },
        { key: "match_title", label: "Title contains", type: "text", placeholder: "Pull request", maxLength: 200 },
        { key: "app", label: "App", type: "text", required: true, placeholder: "VS Code", maxLength: 200 },
        { key: "category", label: "Category", type: "text", required: true, placeholder: "coding", maxLength: 80 },
        { key: "area", label: "Area", type: "area" },
        { key: "confidence", label: "Confidence", type: "number", min: 0, max: 1, step: 0.05, hint: "0.4 and up counts toward your day without review." },
        { key: "enabled", label: "Enabled", type: "checkbox" },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing: r, body: { match_wm_class: opt(v.match_wm_class), match_exe: opt(v.match_exe), match_title: opt(v.match_title), app: String(v.app).trim(), category: String(v.category).trim(), area: v.area as IngestRule["area"], confidence: Number(v.confidence), enabled: !!v.enabled } })}
      onDelete={r ? () => del.mutateAsync(r) : undefined}
    />
  );
}

function ContextsCard() {
  const q = useWorkContexts();
  const devices = useDeviceList();
  const [edit, setEdit] = useState<WorkContext | "new" | null>(null);
  const list = q.data ?? [];
  const devName = (id: string) => devices.data?.find((d) => d.id === id)?.name ?? "a device";
  return (
    <Card id="work-tracking">
      <CardHeader title="Work tracking" icon={<Briefcase className="size-4" />} subtitle="How work time is measured, and how much a workday aims for." action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setEdit("new")}>New</Button>} />
      <div className="px-5 pb-5">
        {q.isPending ? <Skeleton className="h-16" /> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : !list.length ? (
          <EmptyState className="py-6" title="Not set up" body="Observed: the collector on your work devices counts work time. Self-reported: you log it (for work computers you can't install on)." />
        ) : (
          <ul className="divide-y divide-line">
            {list.map((c) => (
              <li key={c.id}>
                <button onClick={() => setEdit(c)} className="flex w-full items-center gap-3 py-3 text-left">
                  <span className="min-w-0 flex-1">
                    <span className="block text-sm text-text">{c.name}{c.employer ? <span className="text-text-3"> · {c.employer}</span> : null}</span>
                    <span className="text-[12px] text-text-3">
                      {c.evidence_mode === "observed" ? `Observed${c.device_ids?.length ? ` on ${c.device_ids.map(devName).join(", ")}` : ""}` : "Self-reported"}
                      {" · "}{Math.round(((c.workday_target_min ?? 0) / 60) * 10) / 10} h a workday
                      {c.active_from || c.active_to ? ` · ${c.active_from ?? "…"} to ${c.active_to ?? "now"}` : ""}
                    </span>
                  </span>
                </button>
              </li>
            ))}
          </ul>
        )}
      </div>
      {edit ? <ContextSheet existing={edit === "new" ? undefined : edit} devices={(devices.data ?? []).filter((d) => !d.revoked_at)} onClose={() => setEdit(null)} /> : null}
    </Card>
  );
}

function ContextSheet({ existing, devices, onClose }: { existing?: WorkContext; devices: { id: string; name: string }[]; onClose: () => void }) {
  const save = useSaveWorkContext();
  const del = useDeleteWorkContext();
  const c = existing;
  return (
    <FormSheet
      id="context-form"
      title={c ? "Edit work tracking" : "Set up work tracking"}
      submitLabel={c ? "Save" : "Add"}
      onClose={onClose}
      initial={{ name: c?.name ?? "", employer: c?.employer ?? "", evidence_mode: c?.evidence_mode ?? "observed", device_ids: c?.device_ids ?? [], hours: String(Math.round(((c?.workday_target_min ?? 480) / 60) * 10) / 10), active_from: c?.active_from ?? "", active_to: c?.active_to ?? "" }}
      validate={(v): Record<string, string> => (!(Number(v.hours) > 0 && Number(v.hours) <= 16) ? { hours: "Between 0 and 16 hours" } : v.active_from && v.active_to && String(v.active_to) < String(v.active_from) ? { active_to: "Ends before it starts" } : {})}
      fields={[
        { key: "name", label: "Name", type: "text", required: true, maxLength: 200, placeholder: "Day job" },
        { key: "employer", label: "Employer", type: "text", maxLength: 200 },
        { key: "evidence_mode", label: "How it's measured", type: "segmented", options: [{ value: "observed", label: "Observed by the collector" }, { value: "self_reported", label: "Self-reported" }] },
        { key: "device_ids", label: "On these devices (observed)", type: "chips", options: devices.map((d) => ({ value: d.id, label: d.name })), hint: devices.length ? "None selected means all your devices." : "Register a device under Connections first." },
        { key: "hours", label: "Workday target (hours)", type: "number", min: 0.5, max: 16, step: 0.5 },
        { key: "active_from", label: "From", type: "date" },
        { key: "active_to", label: "Until", type: "date" },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing: c, body: { name: String(v.name).trim(), employer: opt(v.employer), evidence_mode: v.evidence_mode as WorkContext["evidence_mode"], device_ids: v.evidence_mode === "observed" ? (v.device_ids as string[]) : [], workday_target_min: Math.round(Number(v.hours) * 60), active_from: opt(v.active_from), active_to: opt(v.active_to) } })}
      onDelete={c ? () => del.mutateAsync(c) : undefined}
    />
  );
}
