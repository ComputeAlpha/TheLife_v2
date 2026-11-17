import { useState } from "react";
import { BellRing, Check, Globe, Link2, Monitor, Plus, Repeat, Send, X } from "lucide-react";
import { useUser } from "@/api/auth";
import { useDeleteReminder, useReminderList, useSaveReminder } from "@/api/assistant";
import type { Reminder } from "@/api/types";
import { dayOf } from "@/lib/calendar/days";
import { describeRule } from "@/lib/rrule";
import { fmt, fromLocalInput, localDay, relative, toLocalInput } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button, IconButton } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { FormSheet } from "../common/FormSheet";
import { OriginBadge } from "../common/OriginBadge";
import { RecurrenceBuilder } from "../common/RecurrenceBuilder";
import { SectionLabel, errText } from "../plan/shared";

const CHANNEL = { web: { label: "Web", icon: Globe }, telegram: { label: "Telegram", icon: Send }, desktop: { label: "Desktop", icon: Monitor } } as const;
const STATUS_TONE = { pending: "accent", sent: "neutral", done: "good", cancelled: "neutral" } as const;

export function RemindersPage() {
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const q = useReminderList();
  const save = useSaveReminder();
  const [edit, setEdit] = useState<Reminder | "new" | null>(null);
  const all = [...(q.data ?? [])].sort((a, b) => a.due_at.localeCompare(b.due_at));
  const pending = all.filter((r) => r.status === "pending" || !r.status);
  const past = all.filter((r) => r.status && r.status !== "pending").reverse().slice(0, 50);
  const setStatus = (r: Reminder, status: "done" | "cancelled") =>
    save.mutate({ existing: r, body: { status } }, { onSuccess: () => toast(status === "done" ? "Done" : "Cancelled"), onError: (e) => toast.error(errText(e)) });

  const row = (r: Reminder) => {
    const ch = r.channel_pref ? CHANNEL[r.channel_pref] : null;
    const overdue = (r.status ?? "pending") === "pending" && Date.parse(r.due_at) < now;
    return (
      <li key={r.id} className="flex items-center gap-3 rounded-xl px-3 py-3 hover:bg-surface-2/60">
        <BellRing className={cn("size-4 shrink-0", overdue ? "text-warn" : "text-text-3")} aria-hidden />
        <button onClick={() => setEdit(r)} className="min-w-0 flex-1 text-left">
          <span className={cn("block truncate text-sm", r.status === "done" || r.status === "cancelled" ? "text-text-3 line-through" : "text-text")}>{r.text}</span>
          <span className="mt-0.5 flex flex-wrap items-center gap-x-3 text-[12px] text-text-3">
            <span className={overdue ? "text-warn" : undefined} title={fmt(r.due_at, tz, "EEE d MMM, HH:mm")}>{fmt(r.due_at, tz, dayOf(Date.parse(r.due_at), tz) === localDay(now, tz, 0) ? "'today' HH:mm" : "EEE d MMM, HH:mm")} · {relative(r.due_at, now)}</span>
            {r.rrule ? <span className="flex items-center gap-1 first-letter:uppercase"><Repeat className="size-3" aria-hidden />{describeRule(r.rrule)}</span> : null}
            {ch ? <span className="flex items-center gap-1"><ch.icon className="size-3" aria-hidden />{ch.label}</span> : null}
            {r.link_type ? <span className="flex items-center gap-1"><Link2 className="size-3" aria-hidden />{r.link_type.replace(/_/g, " ")}</span> : null}
          </span>
        </button>
        <OriginBadge row={r} />
        {(r.status ?? "pending") === "pending" ? (
          <span className="flex shrink-0 gap-0.5">
            <IconButton size="sm" label={`Done: ${r.text}`} onClick={() => setStatus(r, "done")}><Check className="size-4" /></IconButton>
            <IconButton size="sm" label={`Cancel: ${r.text}`} onClick={() => setStatus(r, "cancelled")}><X className="size-4" /></IconButton>
          </span>
        ) : <Badge tone={STATUS_TONE[r.status ?? "pending"]} className="capitalize">{r.status}</Badge>}
      </li>
    );
  };

  return (
    <>
      <div className="mb-4 flex items-center justify-between gap-3">
        <p className="text-sm text-text-2">Nudges at a time, on the channel you prefer. Ask the assistant in chat, or add one here.</p>
        <Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setEdit("new")}>New reminder</Button>
      </div>
      {q.isPending ? <Skeleton className="h-40" /> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : !all.length ? (
        <Card><EmptyState art={<BellRing className="size-10 text-accent" />} title="No reminders" body="“Remind me to call mom at 8” in chat works too." action={<Button variant="primary" onClick={() => setEdit("new")}>Add a reminder</Button>} /></Card>
      ) : (
        <Card className="p-2">
          <div className="px-3 pt-2"><SectionLabel>Coming up</SectionLabel></div>
          {pending.length ? <ul className="divide-y divide-line" aria-label="Pending reminders">{pending.map(row)}</ul> : <p className="px-3 pb-3 text-[13px] text-text-3">Nothing pending.</p>}
          {past.length ? (
            <>
              <div className="px-3"><SectionLabel>Earlier</SectionLabel></div>
              <ul className="divide-y divide-line" aria-label="Earlier reminders">{past.map(row)}</ul>
            </>
          ) : null}
        </Card>
      )}
      {edit ? <ReminderSheet existing={edit === "new" ? undefined : edit} tz={tz} now={now} onClose={() => setEdit(null)} /> : null}
    </>
  );
}

function ReminderSheet({ existing, tz, now, onClose }: { existing?: Reminder; tz: string; now: number; onClose: () => void }) {
  const save = useSaveReminder();
  const del = useDeleteReminder();
  const [rrule, setRrule] = useState<string | null>(existing?.rrule ?? null);
  const [due, setDue] = useState(existing?.due_at ?? new Date(Math.ceil((now + 3_600_000) / 900_000) * 900_000).toISOString());
  const today = localDay(now, tz, 0);
  return (
    <FormSheet
      id="reminder-form"
      title={existing ? "Edit reminder" : "New reminder"}
      submitLabel={existing ? "Save" : "Add reminder"}
      onClose={onClose}
      initial={{ text: existing?.text ?? "", due_at: toLocalInput(due, tz), channel_pref: existing?.channel_pref ?? "", status: existing?.status ?? "pending" }}
      validate={(v): Record<string, string> => (fromLocalInput(String(v.due_at), tz) ? {} : { due_at: "Pick a time" })}
      onChange={(v) => { const iso = fromLocalInput(String(v.due_at), tz); if (iso) setDue(iso); }}
      fields={[
        { key: "text", label: "Remind me to", type: "text", required: true, maxLength: 500, placeholder: "Call mom" },
        { key: "due_at", label: "When", type: "datetime" },
        { key: "channel_pref", label: "Where", type: "select", options: [{ value: "", label: "Wherever I am (default)" }, { value: "web", label: "Web" }, { value: "telegram", label: "Telegram" }, { value: "desktop", label: "Desktop (collector)" }] },
        ...(existing ? [{ key: "status", label: "Status", type: "select" as const, options: [{ value: "pending", label: "Pending" }, { value: "sent", label: "Sent" }, { value: "done", label: "Done" }, { value: "cancelled", label: "Cancelled" }] }] : []),
      ]}
      onSubmit={(v) => save.mutateAsync({
        existing,
        body: { text: String(v.text).trim(), due_at: fromLocalInput(String(v.due_at), tz)!, rrule, channel_pref: (v.channel_pref || null) as Reminder["channel_pref"], ...(existing ? { status: v.status as Reminder["status"] } : {}) },
      })}
      onDelete={existing ? () => del.mutateAsync(existing) : undefined}
    >
      <RecurrenceBuilder value={rrule} onChange={setRrule} startDay={dayOf(Date.parse(due), tz)} today={today} />
    </FormSheet>
  );
}
