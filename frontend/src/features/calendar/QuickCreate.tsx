import { useState, type FormEvent } from "react";
import { BookOpen, CalendarClock, CheckSquare, Gamepad2 } from "lucide-react";
import { useCreateEvent, useCreateSession, useCreateSlot, useCreateTask, useEntertainmentItems, useSkillResources } from "@/api/calendar";
import { ApiError } from "@/api/errors";
import type { Area } from "@/api/types";
import { Dialog } from "@/ui/Dialog";
import { Button } from "@/ui/Button";
import { Field, Input, NativeSelect, Textarea } from "@/ui/Field";
import { AreaPicker } from "@/ui/AreaPicker";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { fromLocalInput, toLocalInput } from "@/lib/time";
import { dayOf, dayStartMs, type Day } from "@/lib/calendar/days";
import { RecurrenceField } from "./RecurrenceField";

export type CreateKind = "event" | "task" | "skill_session" | "entertainment_slot";

export interface CreateDraft {
  start: number; // ms
  end: number;
  allDay?: boolean;
  kind?: CreateKind;
}

const KINDS: { kind: CreateKind; label: string; icon: typeof CalendarClock }[] = [
  { kind: "event", label: "Event", icon: CalendarClock },
  { kind: "task", label: "Task", icon: CheckSquare },
  { kind: "skill_session", label: "Study", icon: BookOpen },
  { kind: "entertainment_slot", label: "Fun", icon: Gamepad2 },
];

export function QuickCreate({ draft, tz, onClose }: { draft: CreateDraft | null; tz: string; onClose: () => void }) {
  return (
    <Dialog open={!!draft} onOpenChange={(o) => !o && onClose()} title="New" className="w-[min(560px,calc(100vw-2rem))]">
      {draft ? <CreateForm key={`${draft.start}-${draft.kind}`} draft={draft} tz={tz} onDone={onClose} /> : null}
    </Dialog>
  );
}

function CreateForm({ draft, tz, onDone }: { draft: CreateDraft; tz: string; onDone: () => void }) {
  const [kind, setKind] = useState<CreateKind>(draft.kind ?? "event");
  const [title, setTitle] = useState("");
  const [start, setStart] = useState(toLocalInput(draft.start, tz));
  const [end, setEnd] = useState(toLocalInput(draft.end, tz));
  const [allDay, setAllDay] = useState(!!draft.allDay);
  const [area, setArea] = useState<Area | null>(null);
  const [rrule, setRrule] = useState<string | null>(null);
  const [location, setLocation] = useState("");
  const [notes, setNotes] = useState("");
  const [resourceId, setResourceId] = useState("");
  const [itemId, setItemId] = useState("");
  const resources = useSkillResources();
  const items = useEntertainmentItems();
  const ev = useCreateEvent();
  const task = useCreateTask();
  const session = useCreateSession();
  const slot = useCreateSlot();
  const pending = ev.isPending || task.isPending || session.isPending || slot.isPending;
  const err = [ev.error, task.error, session.error, slot.error].find(Boolean);
  const fieldErrors = err instanceof ApiError ? err.fieldErrors() : {};

  const startIso = fromLocalInput(start, tz);
  const endIso = fromLocalInput(end, tz);
  const startDay: Day = startIso ? dayOf(Date.parse(startIso), tz) : dayOf(draft.start, tz);
  const timesOk = !!startIso && !!endIso && (kind === "task" || Date.parse(endIso) > Date.parse(startIso));

  const ok = () => {
    toast.success("Added to your calendar");
    onDone();
  };
  const submit = (e: FormEvent) => {
    e.preventDefault();
    if (!timesOk) return;
    if (kind === "event") {
      const s = allDay ? new Date(dayStartMs(startDay, tz)).toISOString() : startIso!;
      const en = allDay ? new Date(dayStartMs(dayOf(Date.parse(endIso!) - 1, tz), tz) + 86_400_000).toISOString() : endIso!;
      ev.mutate({ title: title.trim(), starts_at: s, ends_at: en, all_day: allDay, area, rrule, location: location.trim() || null, notes: notes.trim() || null }, { onSuccess: ok });
    } else if (kind === "task") {
      const body = rrule
        ? { title: title.trim(), area: area ?? "personal", rrule, rrule_dtstart: startIso! }
        : { title: title.trim(), area: area ?? "personal", due_at: startIso!, ...(endIso && Date.parse(endIso) > Date.parse(startIso!) ? { estimate_min: Math.min(1440, Math.round((Date.parse(endIso) - Date.parse(startIso!)) / 60000)) } : {}) };
      task.mutate(body, { onSuccess: ok });
    } else if (kind === "skill_session") {
      session.mutate({ resource_id: resourceId, started_at: startIso!, ended_at: endIso!, ...(notes.trim() ? { notes: notes.trim() } : {}) }, { onSuccess: ok });
    } else {
      slot.mutate({ item_id: itemId || null, starts_at: startIso!, ends_at: endIso!, ...(notes.trim() ? { notes: notes.trim() } : {}) }, { onSuccess: ok });
    }
  };

  const needsTitle = kind === "event" || kind === "task";
  const canSubmit = timesOk && (!needsTitle || title.trim()) && (kind !== "skill_session" || resourceId);

  return (
    <form onSubmit={submit} className="space-y-4">
      <div className="grid grid-cols-4 gap-1 rounded-xl border border-line p-1" role="tablist" aria-label="What to add">
        {KINDS.map((k) => (
          <button
            key={k.kind}
            type="button"
            role="tab"
            aria-selected={kind === k.kind}
            onClick={() => setKind(k.kind)}
            className={cn(
              "flex items-center justify-center gap-1.5 rounded-lg py-2 text-[13px] transition-colors",
              kind === k.kind ? "bg-accent/12 text-text shadow-[inset_0_0_0_1px_rgb(34_211_238/.45)]" : "text-text-2 hover:text-text",
            )}
          >
            <k.icon className={cn("size-4", kind === k.kind ? "text-accent" : "")} aria-hidden />
            {k.label}
          </button>
        ))}
      </div>

      {needsTitle ? (
        <Field label="Title" error={fieldErrors.title}>
          {(p) => <Input {...p} autoFocus value={title} maxLength={200} onChange={(e) => setTitle(e.target.value)} placeholder={kind === "task" ? "Read 20 pages" : "Gym"} />}
        </Field>
      ) : null}

      {kind === "skill_session" ? (
        <Field label="Book or course" error={fieldErrors.resource_id} hint={resources.data?.items.length === 0 ? "Add a book or course in Skills first." : undefined}>
          {(p) => (
            <NativeSelect {...p} value={resourceId} onChange={(e) => setResourceId(e.target.value)}>
              <option value="">Choose…</option>
              {resources.data?.items.map((r) => <option key={r.id} value={r.id}>{r.title}</option>)}
            </NativeSelect>
          )}
        </Field>
      ) : null}
      {kind === "entertainment_slot" ? (
        <Field label="What" hint="Optional: a game, movie or series from your list.">
          {(p) => (
            <NativeSelect {...p} value={itemId} onChange={(e) => setItemId(e.target.value)}>
              <option value="">Anything</option>
              {items.data?.items.map((i) => <option key={i.id} value={i.id}>{i.title}</option>)}
            </NativeSelect>
          )}
        </Field>
      ) : null}

      <div className="grid gap-3 sm:grid-cols-2">
        <Field label={kind === "task" ? (rrule ? "Starts" : "Due") : "Starts"} error={fieldErrors.starts_at ?? fieldErrors.due_at ?? fieldErrors.started_at}>
          {(p) => <Input {...p} type="datetime-local" value={start} onChange={(e) => setStart(e.target.value)} />}
        </Field>
        {kind !== "task" || !rrule ? (
          <Field label={kind === "task" ? "Until (estimate)" : "Ends"} error={fieldErrors.ends_at ?? fieldErrors.ended_at ?? (startIso && endIso && kind !== "task" && !timesOk ? "Ends before it starts." : undefined)}>
            {(p) => <Input {...p} type="datetime-local" value={end} onChange={(e) => setEnd(e.target.value)} />}
          </Field>
        ) : null}
      </div>

      {kind === "event" ? (
        <label className="flex items-center gap-2 text-sm text-text-2">
          <input type="checkbox" checked={allDay} onChange={(e) => setAllDay(e.target.checked)} className="size-4 accent-[var(--color-accent)]" />
          All day
        </label>
      ) : null}

      {kind === "event" || kind === "task" ? (
        <>
          <AreaPicker value={area} onChange={setArea} />
          <RecurrenceField value={rrule} onChange={setRrule} day={startDay} error={fieldErrors.rrule} />
        </>
      ) : null}
      {kind === "event" ? (
        <Field label="Location">{(p) => <Input {...p} value={location} maxLength={200} onChange={(e) => setLocation(e.target.value)} />}</Field>
      ) : null}
      {kind !== "task" ? (
        <Field label="Notes">{(p) => <Textarea {...p} rows={2} className="min-h-0" value={notes} onChange={(e) => setNotes(e.target.value)} />}</Field>
      ) : null}

      {err && !Object.keys(fieldErrors).length ? <p role="alert" className="text-[13px] text-alert-text">{(err as ApiError).summary}</p> : null}
      <div className="flex justify-end gap-2 pt-1">
        <Button variant="ghost" onClick={onDone}>Cancel</Button>
        <Button type="submit" variant="primary" loading={pending} disabled={!canSubmit}>Add</Button>
      </div>
    </form>
  );
}
