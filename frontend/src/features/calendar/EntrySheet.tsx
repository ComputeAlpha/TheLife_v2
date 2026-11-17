import { useState, type FormEvent } from "react";
import { Link } from "react-router";
import { Check, Clock, ExternalLink, MapPin, Repeat, SkipForward, Trash2, Undo2 } from "lucide-react";
import type { Area, CalendarEntry, CalendarEvent } from "@/api/types";
import { useDeleteEvent, useDeleteTimed, useEvent, usePatchEvent, usePatchTimed, useTaskAction, useTask } from "@/api/calendar";
import { ApiError } from "@/api/errors";
import { Sheet } from "@/ui/Dialog";
import { Button } from "@/ui/Button";
import { Badge } from "@/ui/Badge";
import { Field, Input, Textarea } from "@/ui/Field";
import { AreaPicker } from "@/ui/AreaPicker";
import { Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { areaMeta } from "@/lib/areas";
import { describeRule } from "@/lib/rrule";
import { fmt, fromLocalInput, timeRange, toLocalInput } from "@/lib/time";
import { dayOf } from "@/lib/calendar/days";
import { KIND_LABEL, isPoint } from "@/lib/calendar/entries";
import { KIND_ICON, areaVars, isRecurring, statusLabel } from "./EntryChip";
import { RecurrenceField } from "./RecurrenceField";

export function EntrySheet({ entry, tz, onClose }: { entry: CalendarEntry | null; tz: string; onClose: () => void }) {
  return (
    <Sheet open={!!entry} onOpenChange={(o) => !o && onClose()} title={entry ? <SheetTitle entry={entry} /> : ""}>
      {entry ? <Body key={`${entry.kind}${entry.id}${entry.occurs_on}`} entry={entry} tz={tz} onClose={onClose} /> : null}
    </Sheet>
  );
}

function SheetTitle({ entry }: { entry: CalendarEntry }) {
  const Icon = KIND_ICON[entry.kind];
  return (
    <span className="flex items-center gap-2.5" style={areaVars(entry.area)}>
      <span className="grid size-8 place-items-center rounded-lg bg-[color-mix(in_oklab,var(--a)_18%,transparent)]">
        <Icon className="size-4 text-[var(--a)]" aria-hidden />
      </span>
      <span className="min-w-0 truncate">{entry.title}</span>
    </span>
  );
}

function When({ entry, tz }: { entry: CalendarEntry; tz: string }) {
  const day = entry.occurs_on ?? dayOf(Date.parse(entry.starts_at), tz);
  const label = entry.all_day
    ? fmt(Date.parse(day + "T12:00:00Z"), "UTC", "EEEE d MMMM") + " · all day"
    : `${fmt(entry.starts_at, tz, "EEEE d MMMM")} · ${timeRange(entry.starts_at, entry.ends_at, tz)}${isPoint(entry) ? " (due)" : ""}`;
  return (
    <p className="flex items-center gap-2 text-sm text-text-2">
      <Clock className="size-4 text-text-3" aria-hidden /> {label}
    </p>
  );
}

function Meta({ entry }: { entry: CalendarEntry }) {
  const a = areaMeta(entry.area);
  const s = statusLabel(entry);
  return (
    <div className="flex flex-wrap gap-1.5">
      <Badge>{KIND_LABEL[entry.kind]}</Badge>
      {entry.area ? (
        <Badge style={areaVars(entry.area)} className="border-[color-mix(in_oklab,var(--a)_40%,transparent)] bg-[color-mix(in_oklab,var(--a)_12%,transparent)] text-text">
          <a.icon className="size-3 text-[var(--a)]" aria-hidden /> {a.label}
        </Badge>
      ) : null}
      {s ? <Badge tone={entry.status === "done" || entry.status === "consumed" ? "good" : entry.status === "missed" ? "alert" : "neutral"}>{s}</Badge> : null}
      {isRecurring(entry) ? <Badge tone="accent"><Repeat className="size-3" /> Repeats</Badge> : null}
    </div>
  );
}

function Body({ entry, tz, onClose }: { entry: CalendarEntry; tz: string; onClose: () => void }) {
  return (
    <div className="space-y-5">
      <When entry={entry} tz={tz} />
      <Meta entry={entry} />
      {entry.kind === "event" ? <EventEditor id={entry.id} tz={tz} recurring={isRecurring(entry)} onClose={onClose} /> : null}
      {entry.kind === "task_occurrence" ? <TaskPanel entry={entry} onClose={onClose} /> : null}
      {entry.kind === "entertainment_slot" || entry.kind === "skill_session" ? <TimedPanel entry={entry} tz={tz} onClose={onClose} /> : null}
    </div>
  );
}

function TaskPanel({ entry, onClose }: { entry: CalendarEntry; onClose: () => void }) {
  const action = useTaskAction();
  const task = useTask(entry.id);
  const [reason, setReason] = useState("");
  const [skipping, setSkipping] = useState(false);
  const recurring = entry.all_day && !!entry.occurs_on;
  const run = (verb: "complete" | "skip" | "reopen") =>
    action.mutate(
      { entry, verb, reason: reason.trim() || undefined },
      {
        onSuccess: () => {
          toast.success(verb === "complete" ? "Nice, done." : verb === "skip" ? "Skipped" : "Reopened");
          onClose();
        },
        onError: (e) => toast.error("Couldn't update the task", e instanceof ApiError ? e.summary : undefined),
      },
    );
  const done = entry.status === "done";
  return (
    <div className="space-y-4">
      {task.data?.notes ? <p className="whitespace-pre-wrap rounded-lg border border-line p-3 text-sm text-text-2">{task.data.notes}</p> : null}
      {recurring && task.data?.rrule ? <p className="text-[13px] text-text-2">Repeats {describeRule(task.data.rrule)}. Completing or skipping affects {entry.occurs_on} only.</p> : null}
      <div className="flex flex-wrap gap-2">
        {!done ? (
          <Button variant="primary" icon={<Check className="size-4" />} loading={action.isPending && action.variables?.verb === "complete"} onClick={() => run("complete")}>
            Mark done
          </Button>
        ) : !recurring ? (
          <Button icon={<Undo2 className="size-4" />} onClick={() => run("reopen")} loading={action.isPending}>Reopen</Button>
        ) : null}
        {!done && entry.status !== "skipped" ? (
          <Button icon={<SkipForward className="size-4" />} onClick={() => setSkipping((s) => !s)}>Skip</Button>
        ) : null}
        <Link to={`/tasks?open=${entry.id}`} className="inline-flex h-10 items-center gap-1.5 px-3 text-sm text-text-2 hover:text-accent">
          Open task <ExternalLink className="size-3.5" />
        </Link>
      </div>
      {skipping ? (
        <div className="space-y-2 rounded-xl border border-line p-3">
          <Field label="Why skip? (optional)">{(p) => <Input {...p} value={reason} maxLength={500} onChange={(e) => setReason(e.target.value)} placeholder="Travelling" />}</Field>
          <Button size="sm" onClick={() => run("skip")} loading={action.isPending}>Skip {recurring ? "this day" : "task"}</Button>
        </div>
      ) : null}
    </div>
  );
}

function TimedPanel({ entry, tz, onClose }: { entry: CalendarEntry; tz: string; onClose: () => void }) {
  const kind = entry.kind as "entertainment_slot" | "skill_session";
  const patch = usePatchTimed();
  const del = useDeleteTimed();
  const [start, setStart] = useState(toLocalInput(entry.starts_at, tz));
  const [end, setEnd] = useState(toLocalInput(entry.ends_at, tz));
  const s = fromLocalInput(start, tz);
  const e = fromLocalInput(end, tz);
  const changed = s !== new Date(entry.starts_at).toISOString() || e !== new Date(entry.ends_at).toISOString();
  const valid = !!s && !!e && Date.parse(e) > Date.parse(s);
  return (
    <div className="space-y-4">
      <div className="grid gap-3 sm:grid-cols-2">
        <Field label="Starts">{(p) => <Input {...p} type="datetime-local" value={start} onChange={(ev) => setStart(ev.target.value)} />}</Field>
        <Field label="Ends" error={s && e && !valid ? "Ends before it starts." : undefined}>{(p) => <Input {...p} type="datetime-local" value={end} onChange={(ev) => setEnd(ev.target.value)} />}</Field>
      </div>
      <div className="flex flex-wrap gap-2">
        <Button variant="primary" disabled={!changed || !valid} loading={patch.isPending}
          onClick={() => patch.mutate({ kind, id: entry.id, patch: { starts_at: s, ends_at: e } }, { onSuccess: () => { toast.success("Moved"); onClose(); } })}>
          Save time
        </Button>
        {kind === "entertainment_slot" && entry.status !== "consumed" ? (
          <Button icon={<Check className="size-4" />} loading={patch.isPending}
            onClick={() => patch.mutate({ kind, id: entry.id, patch: { consumed: true } }, { onSuccess: () => { toast.success("Enjoyed"); onClose(); } })}>
            Mark enjoyed
          </Button>
        ) : null}
        <Button variant="danger" icon={<Trash2 className="size-4" />} loading={del.isPending}
          onClick={() => del.mutate({ kind, id: entry.id }, { onSuccess: () => { toast("Removed from the calendar"); onClose(); } })}>
          Delete
        </Button>
      </div>
    </div>
  );
}

function EventEditor({ id, tz, recurring, onClose }: { id: string; tz: string; recurring: boolean; onClose: () => void }) {
  const ev = useEvent(id);
  if (ev.isPending) return <div className="space-y-3"><Skeleton className="h-10" /><Skeleton className="h-10" /><Skeleton className="h-24" /></div>;
  if (!ev.data) return <p className="text-sm text-alert-text">{ev.error instanceof ApiError ? ev.error.summary : "Couldn't load the event."}</p>;
  return <EventForm key={ev.data.version} ev={ev.data} tz={tz} recurring={recurring} onClose={onClose} />;
}

function EventForm({ ev, tz, recurring, onClose }: { ev: CalendarEvent; tz: string; recurring: boolean; onClose: () => void }) {
  const patch = usePatchEvent();
  const del = useDeleteEvent();
  const [title, setTitle] = useState(ev.title);
  const [start, setStart] = useState(toLocalInput(ev.starts_at, tz));
  const [end, setEnd] = useState(toLocalInput(ev.ends_at, tz));
  const [area, setArea] = useState<Area | null>((ev.area as Area | null) ?? null);
  const [rrule, setRrule] = useState<string | null>(ev.rrule ?? null);
  const [location, setLocation] = useState(ev.location ?? "");
  const [notes, setNotes] = useState(ev.notes ?? "");
  const [confirmDelete, setConfirmDelete] = useState(false);
  const errors = patch.error instanceof ApiError ? patch.error.fieldErrors() : {};
  const s = fromLocalInput(start, tz);
  const e = fromLocalInput(end, tz);

  const body: Record<string, unknown> = {};
  if (title.trim() !== ev.title) body.title = title.trim();
  if (s && s !== new Date(ev.starts_at).toISOString()) body.starts_at = s;
  if (e && e !== new Date(ev.ends_at).toISOString()) body.ends_at = e;
  if (area !== (ev.area ?? null)) body.area = area;
  if ((rrule ?? null) !== (ev.rrule ?? null)) body.rrule = rrule;
  if (location.trim() !== (ev.location ?? "")) body.location = location.trim() || null;
  if (notes.trim() !== (ev.notes ?? "")) body.notes = notes.trim() || null;
  const dirty = Object.keys(body).length > 0;
  const valid = !!title.trim() && !!s && !!e && Date.parse(e) > Date.parse(s);

  const save = (f: FormEvent) => {
    f.preventDefault();
    if (!dirty || !valid) return;
    patch.mutate(
      { id: ev.id, patch: body, version: ev.version },
      {
        onSuccess: () => { toast.success("Event updated"); onClose(); },
        onError: (err) => {
          if (err instanceof ApiError && (err.status === 412 || err.status === 409))
            toast.error("Changed elsewhere", "Someone (or the assistant) edited this event. Reopen it to see the latest.");
        },
      },
    );
  };

  return (
    <form onSubmit={save} className="space-y-4">
      {recurring ? (
        <p className="rounded-lg border border-accent/30 bg-accent/5 px-3 py-2 text-[13px] text-text-2">
          This event repeats. Changes apply to the whole series.
        </p>
      ) : null}
      <Field label="Title" error={errors.title}>{(p) => <Input {...p} value={title} maxLength={200} onChange={(x) => setTitle(x.target.value)} />}</Field>
      <div className="grid gap-3 sm:grid-cols-2">
        <Field label="Starts" error={errors.starts_at}>{(p) => <Input {...p} type="datetime-local" value={start} onChange={(x) => setStart(x.target.value)} />}</Field>
        <Field label="Ends" error={errors.ends_at ?? (s && e && Date.parse(e) <= Date.parse(s) ? "Ends before it starts." : undefined)}>
          {(p) => <Input {...p} type="datetime-local" value={end} onChange={(x) => setEnd(x.target.value)} />}
        </Field>
      </div>
      <AreaPicker value={area} onChange={setArea} />
      <RecurrenceField value={rrule} onChange={setRrule} day={s ? dayOf(Date.parse(s), tz) : dayOf(Date.parse(ev.starts_at), tz)} error={errors.rrule} />
      <Field label={<span className="inline-flex items-center gap-1"><MapPin className="size-3.5" /> Location</span>}>
        {(p) => <Input {...p} value={location} maxLength={200} onChange={(x) => setLocation(x.target.value)} />}
      </Field>
      <Field label="Notes">{(p) => <Textarea {...p} value={notes} onChange={(x) => setNotes(x.target.value)} />}</Field>
      {ev.origin !== "direct" ? <p className="text-[12.5px] text-text-3">Added by the assistant{ev.origin === "user_stated" ? " from something you said" : ""}.</p> : null}
      <div className="flex flex-wrap items-center justify-between gap-2 border-t border-line pt-4">
        {confirmDelete ? (
          <span className="flex items-center gap-2">
            <Button variant="danger" size="sm" loading={del.isPending}
              onClick={() => del.mutate({ id: ev.id, version: ev.version }, { onSuccess: () => { toast("Event deleted"); onClose(); } })}>
              Delete{recurring ? " series" : ""}
            </Button>
            <Button variant="ghost" size="sm" onClick={() => setConfirmDelete(false)}>Keep</Button>
          </span>
        ) : (
          <Button variant="ghost" size="sm" icon={<Trash2 className="size-4" />} onClick={() => setConfirmDelete(true)}>Delete</Button>
        )}
        <Button type="submit" variant="primary" disabled={!dirty || !valid} loading={patch.isPending}>Save</Button>
      </div>
    </form>
  );
}
