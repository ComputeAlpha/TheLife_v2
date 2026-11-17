import { useState } from "react";
import { useQuery } from "@tanstack/react-query";
import { Check, Clock, ShieldAlert, ShieldCheck, X } from "lucide-react";
import type { Approval } from "@/api/types";
import { useDecideApproval } from "@/api/today";
import { useApproval } from "@/api/chat";
import { ApiError } from "@/api/errors";
import { toolLabel } from "@/api/live";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Input } from "@/ui/Field";
import { Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { relative } from "@/lib/time";
import { useNow } from "@/lib/useNow";

const STATUS: Record<string, { label: string; tone: "warn" | "good" | "neutral" | "alert" }> = {
  pending: { label: "Waiting for you", tone: "warn" },
  approved: { label: "Approved", tone: "good" },
  rejected: { label: "Rejected", tone: "neutral" },
  expired: { label: "Expired", tone: "neutral" },
  stale: { label: "Out of date", tone: "neutral" },
  failed: { label: "Failed", tone: "alert" },
};

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

/** The arguments worth reading: no ids (the target is named separately) and no bookkeeping. */
export function fields(args: unknown): [string, string][] {
  if (!args || typeof args !== "object") return [];
  return Object.entries(args as Record<string, unknown>)
    .filter(([k, v]) => !["expected_version", "id"].includes(k) && !(typeof v === "string" && UUID.test(v)))
    .map(([k, v]) => [k.replace(/_/g, " "), typeof v === "string" ? v : JSON.stringify(v)]);
}

/** "Delete a task (soft delete). Target: Task 0190… Arguments: {…}" → "Delete a task (soft delete)." */
export function plainSummary(s: string | null | undefined): string | null {
  if (!s) return null;
  const cut = s.search(/\s(Target|Arguments):/);
  return (cut > 0 ? s.slice(0, cut) : s).trim();
}

/** Approval target_type (the contract's schema name) → its collection path. */
const COLLECTION: Record<string, string> = {
  Task: "tasks", CalendarEvent: "events", Goal: "goals", Plan: "plans", Target: "targets", Reminder: "reminders",
  Routine: "routines", Activity: "activities", Detection: "detections", MemoryFact: "memory/facts",
  EntertainmentItem: "entertainment/items", EntertainmentSlot: "entertainment/slots", SkillResource: "skills/resources",
  SkillSession: "skills/sessions", CourseSection: "skills/sections", WorkProject: "work/projects", WorkRole: "work/roles",
  WorkDeliverable: "work/deliverables", WorkResponsibility: "work/responsibilities", WorkContext: "work-contexts",
  DayProfile: "day-profiles", DayProfileOverride: "day-profile-overrides", IngestRule: "ingest/rules",
  Device: "devices", ApiKey: "api-keys",
};

/** The approval's target by name ("Old journal cleanup"), when it can be read. */
function useTargetName(type: string | null | undefined, id: string | null | undefined, enabled: boolean) {
  const coll = type ? COLLECTION[type] ?? null : null;
  return useQuery({
    queryKey: ["approval-target", type, id],
    enabled: enabled && !!coll && !!id,
    staleTime: 60_000,
    retry: false,
    queryFn: async () => {
      const r = await fetch(`/api/v1/${coll}/${id}`, { credentials: "same-origin" });
      if (!r.ok) return null;
      const j = (await r.json()) as Record<string, unknown>;
      const name = j.title ?? j.name ?? j.text ?? j.statement ?? j.summary;
      return typeof name === "string" ? name : null;
    },
  });
}

export function ApprovalById({ id, highlight }: { id: string; highlight?: boolean }) {
  const q = useApproval(id);
  if (q.isPending) return <Skeleton className="h-28" />;
  if (!q.data) return null;
  return <ApprovalCard a={q.data} highlight={highlight} anchor />;
}

/** `anchor`: this card is the target of /chat#approval-<id> links (only one per page may carry the id). */
export function ApprovalCard({ a, highlight, anchor }: { a: Approval; highlight?: boolean; anchor?: boolean }) {
  const decide = useDecideApproval();
  const now = useNow(30_000);
  const [rejecting, setRejecting] = useState(false);
  const [reason, setReason] = useState("");
  const s = STATUS[a.status ?? "pending"] ?? STATUS.pending!;
  // Only while pending: a decided change may have removed its target.
  const target = useTargetName(a.target_type, a.target_id, a.status === "pending");
  const summary = plainSummary(a.summary);
  const pending = a.status === "pending";
  const run = (verb: "approve" | "reject") =>
    decide.mutate(
      { id: a.id, verb, reason: reason.trim() || undefined },
      {
        onSuccess: (r) => {
          const st = (r as Approval).status;
          if (st === "approved") toast.success("Approved and done");
          else if (st === "stale") toast("Nothing ran", { body: "It changed since the assistant asked." });
          else if (st === "failed") toast.error("It didn't work", (r as Approval).error ?? undefined);
          else toast("Rejected: nothing changed");
          setRejecting(false);
        },
        onError: (e) => toast.error("Couldn't decide", e instanceof ApiError ? e.summary : undefined),
      },
    );
  return (
    <div
      id={anchor ? `approval-${a.id}` : undefined}
      data-approval={a.id}
      className={cn(
        "scroll-mt-24 rounded-xl border p-4 transition-shadow",
        pending ? "border-warn/40 bg-warn/[.03]" : "border-line",
        highlight && "ring-2 ring-accent shadow-[0_0_30px_-8px_rgb(34_211_238/.7)]",
      )}
    >
      <div className="flex items-start gap-3">
        <span className={cn("grid size-9 shrink-0 place-items-center rounded-lg", pending ? "bg-warn/15 text-warn" : "bg-surface-3 text-text-2")}>
          {pending ? <ShieldAlert className="size-4.5" /> : <ShieldCheck className="size-4.5" />}
        </span>
        <div className="min-w-0 flex-1">
          <div className="flex flex-wrap items-center gap-2">
            <p className="font-medium text-text">{a.tool ? toolLabel(a.tool) : "Change"}</p>
            <Badge tone={s.tone}>{s.label}</Badge>
          </div>
          {target.data ? <p className="mt-1 text-[15px] text-text">“{target.data}”</p> : null}
          {summary ? <p className="mt-0.5 text-[13px] text-text-2">{summary}</p> : null}
          {fields(a.args).length ? (
            <dl className="mt-3 grid grid-cols-[max-content_1fr] gap-x-4 gap-y-1 rounded-lg border border-line bg-surface-2 p-3 text-[12.5px]">
              {fields(a.args).map(([k, v]) => (
                <div key={k} className="contents">
                  <dt className="capitalize text-text-3">{k}</dt>
                  <dd className="min-w-0 break-words font-mono text-text">{v}</dd>
                </div>
              ))}
            </dl>
          ) : null}
          {pending && a.expires_at ? (
            <p className="mt-2 flex items-center gap-1 text-[12px] text-text-3"><Clock className="size-3" /> Expires {relative(a.expires_at, now)}</p>
          ) : null}
          {a.decision_reason ? <p className="mt-2 text-[12.5px] text-text-3">Reason: {a.decision_reason}</p> : null}
          {a.error ? <p className="mt-2 text-[12.5px] text-alert-text">{a.error}</p> : null}
          {pending ? (
            <div className="mt-3 space-y-2">
              <div className="flex flex-wrap gap-2">
                <Button size="sm" variant="primary" icon={<Check className="size-3.5" />} loading={decide.isPending && decide.variables?.verb === "approve"} onClick={() => run("approve")}>
                  Approve
                </Button>
                <Button size="sm" icon={<X className="size-3.5" />} onClick={() => (rejecting ? run("reject") : setRejecting(true))} loading={decide.isPending && decide.variables?.verb === "reject"}>
                  {rejecting ? "Reject now" : "Reject"}
                </Button>
              </div>
              {rejecting ? (
                <Input aria-label="Why reject? (optional)" placeholder="Why? (optional, the assistant sees this)" value={reason} maxLength={500} onChange={(e) => setReason(e.target.value)} onKeyDown={(e) => e.key === "Enter" && run("reject")} />
              ) : null}
            </div>
          ) : null}
        </div>
      </div>
    </div>
  );
}
