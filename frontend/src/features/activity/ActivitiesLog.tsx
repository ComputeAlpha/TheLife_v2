import { useState } from "react";
import { Bot, NotebookPen, Plus, Radar, Trash2 } from "lucide-react";
import type { Activity, Area } from "@/api/types";
import { useActivitiesIn, useDeleteActivity, useSaveActivity } from "@/api/review";
import { ApiError } from "@/api/errors";
import { areaMeta } from "@/lib/areas";
import { dayStartMs, type Day } from "@/lib/calendar/days";
import { fmt, fromLocalInput, minutesLabel, toLocalInput } from "@/lib/time";
import { AreaPicker } from "@/ui/AreaPicker";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, Textarea } from "@/ui/Field";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { useNow } from "@/lib/useNow";
import { areaVars } from "../calendar/EntryChip";

const SOURCE: Record<string, { label: string; icon: typeof Bot }> = {
  manual: { label: "Logged", icon: NotebookPen },
  detection: { label: "From a detection", icon: Radar },
  agent: { label: "By the assistant", icon: Bot },
};

export function ActivitiesLog({ day, tz, fromIso, toIso, creating, setCreating }: {
  day: Day; tz: string; fromIso: string; toIso: string; creating: boolean; setCreating: (v: boolean) => void;
}) {
  const q = useActivitiesIn(fromIso, toIso);
  const [editing, setEditing] = useState<Activity | null>(null);
  const items = [...(q.data ?? [])].sort((a, b) => a.started_at.localeCompare(b.started_at));
  const total = items.reduce((m, a) => m + Math.max(0, (Date.parse(a.ended_at) - Date.parse(a.started_at)) / 60_000), 0);

  return (
    <>
      {q.isPending ? (
        <div className="space-y-2">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-16" />)}</div>
      ) : q.error ? (
        <ErrorState error={q.error} retry={() => q.refetch()} />
      ) : !items.length ? (
        <Card>
          <EmptyState
            art={<NotebookPen className="size-10 text-accent" />}
            title="Nothing logged this day"
            body="Log what the collector can't see: a run, a book, a meeting away from the desk. It counts toward targets and scores."
            action={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setCreating(true)}>Log activity</Button>}
          />
        </Card>
      ) : (
        <Card className="p-2">
          <p className="px-3 pb-1 pt-2 text-[12.5px] text-text-3">{items.length} logged · {minutesLabel(Math.round(total))}</p>
          <ul className="divide-y divide-line">
            {items.map((a) => {
              const m = areaMeta(a.area);
              const s = SOURCE[a.source ?? "manual"] ?? SOURCE.manual!;
              const mins = Math.round((Date.parse(a.ended_at) - Date.parse(a.started_at)) / 60_000);
              return (
                <li key={a.id} style={areaVars(a.area)}>
                  <button onClick={() => setEditing(a)} className="flex w-full items-center gap-4 rounded-xl px-3 py-3 text-left hover:bg-surface-2">
                    <span className="w-24 shrink-0 font-mono text-[12.5px] text-text-2">
                      {fmt(a.started_at, tz, "HH:mm")}<span className="text-text-3">–{fmt(a.ended_at, tz, "HH:mm")}</span>
                    </span>
                    <span className="h-9 w-[3px] shrink-0 rounded-full bg-[var(--a)]" aria-hidden />
                    <span className="min-w-0 flex-1">
                      <span className="block truncate text-sm font-medium text-text">{a.title}</span>
                      <span className="flex items-center gap-1.5 text-[12px] text-text-3">
                        <m.icon className="size-3 text-[var(--a)]" aria-hidden /> {m.label}
                        {a.category ? <> · {a.category}</> : null} · {minutesLabel(mins)}
                      </span>
                    </span>
                    <Badge className="hidden sm:inline-flex"><s.icon className="size-3" aria-hidden /> {s.label}</Badge>
                  </button>
                </li>
              );
            })}
          </ul>
        </Card>
      )}
      <ActivitySheet
        key={editing?.id ?? (creating ? "new" : "none")}
        open={creating || !!editing}
        onOpenChange={(o) => { if (!o) { setCreating(false); setEditing(null); } }}
        existing={editing}
        day={day}
        tz={tz}
      />
    </>
  );
}

function ActivitySheet({ open, onOpenChange, existing, day, tz }: {
  open: boolean; onOpenChange: (o: boolean) => void; existing: Activity | null; day: Day; tz: string;
}) {
  const now = useNow();
  const defaultEnd = Math.min(now, dayStartMs(day, tz) + 18 * 3_600_000);
  const [title, setTitle] = useState(existing?.title ?? "");
  const [area, setArea] = useState<Area | null>(existing?.area ?? null);
  const [category, setCategory] = useState(existing?.category ?? "");
  const [start, setStart] = useState(toLocalInput(existing?.started_at ?? defaultEnd - 30 * 60_000, tz));
  const [end, setEnd] = useState(toLocalInput(existing?.ended_at ?? defaultEnd, tz));
  const [notes, setNotes] = useState(existing?.notes ?? "");
  const [confirmDelete, setConfirmDelete] = useState(false);
  const save = useSaveActivity();
  const del = useDeleteActivity();
  const s = fromLocalInput(start, tz);
  const e = fromLocalInput(end, tz);
  const badRange = !s || !e || Date.parse(e) <= Date.parse(s);
  const fieldErr = (name: string) => (save.error instanceof ApiError ? save.error.fieldErrors()[name] : undefined);
  const valid = title.trim() && area && !badRange;

  return (
    <Sheet
      open={open}
      onOpenChange={onOpenChange}
      title={existing ? "Edit activity" : "Log activity"}
      description="Something you did, with when and how long."
      footer={
        <>
          {existing ? (
            confirmDelete ? (
              <Button variant="danger" className="mr-auto" loading={del.isPending} icon={<Trash2 className="size-4" />}
                onClick={() => del.mutate(existing, { onSuccess: () => { toast("Deleted"); onOpenChange(false); }, onError: () => toast.error("Couldn't delete") })}>
                Delete for good
              </Button>
            ) : (
              <Button variant="ghost" className="mr-auto" icon={<Trash2 className="size-4" />} onClick={() => setConfirmDelete(true)}>Delete</Button>
            )
          ) : null}
          <Button variant="ghost" onClick={() => onOpenChange(false)}>Cancel</Button>
          <Button type="submit" form="activity-form" variant="primary" loading={save.isPending} disabled={!valid}>{existing ? "Save" : "Log it"}</Button>
        </>
      }
    >
      <form
        id="activity-form"
        className="space-y-4"
        onSubmit={(ev) => {
          ev.preventDefault();
          if (!valid) return;
          save.mutate(
            { existing: existing ?? undefined, body: { title: title.trim(), area: area!, category: category.trim() || null, started_at: s!, ended_at: e!, notes: notes.trim() || null, ...(existing ? {} : { source: "manual" as const }) } },
            { onSuccess: () => { toast.success(existing ? "Saved" : "Logged"); onOpenChange(false); } },
          );
        }}
      >
        <Field label="What did you do?" error={fieldErr("title")}>
          {(f) => <Input {...f} autoFocus required maxLength={200} value={title} onChange={(ev) => setTitle(ev.target.value)} placeholder="Evening run" />}
        </Field>
        <AreaPicker value={area} onChange={setArea} />
        <Field label="Category" hint="Optional: a short label targets can match, like running or reading.">
          {(f) => <Input {...f} maxLength={80} value={category} onChange={(ev) => setCategory(ev.target.value)} />}
        </Field>
        <div className="grid grid-cols-2 gap-3">
          <Field label="From">{(f) => <Input {...f} type="datetime-local" value={start} onChange={(ev) => setStart(ev.target.value)} />}</Field>
          <Field label="To" error={badRange ? "Ends before it starts" : fieldErr("ended_at")}>{(f) => <Input {...f} type="datetime-local" value={end} onChange={(ev) => setEnd(ev.target.value)} />}</Field>
        </div>
        <Field label="Notes">{(f) => <Textarea {...f} rows={3} value={notes} onChange={(ev) => setNotes(ev.target.value)} />}</Field>
        {save.error && !fieldErr("title") ? <p role="alert" className="text-[13px] text-alert-text">{save.error instanceof ApiError ? save.error.detail ?? save.error.title : "Couldn't save"}</p> : null}
      </form>
    </Sheet>
  );
}
