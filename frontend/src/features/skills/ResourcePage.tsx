import { useMemo, useState } from "react";
import { Link, useNavigate, useParams } from "react-router";
import { ArrowDown, ArrowLeft, ArrowUp, BookOpen, CheckCircle2, Circle, ExternalLink, Flag, MonitorPlay, PencilLine, Plus, Timer, Trash2 } from "lucide-react";
import { useUser } from "@/api/auth";
import { useGoals } from "@/api/plan";
import { useDeleteSection, useDeleteSession, useResources, useSaveResource, useSaveSection, useSaveSession, useSections, useSessions } from "@/api/skills";
import type { CourseSection, SkillResource, SkillSession } from "@/api/types";
import { fmt, fromLocalInput, minutesLabel, toLocalInput } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button, IconButton } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, NativeSelect, Textarea } from "@/ui/Field";
import { Ring } from "@/ui/Ring";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { OriginBadge } from "../common/OriginBadge";
import { Page } from "../common/Page";
import { DeleteButton, errText } from "../plan/shared";
import { ResourceSheet } from "./ResourceSheet";
import { progressOf, progressText } from "./SkillsPage";

export function ResourcePage() {
  const { id } = useParams();
  const q = useResources();
  const navigate = useNavigate();
  const r = q.data?.find((x) => x.id === id);
  if (q.isPending) return <Page title="…"><Skeleton className="h-64" /></Page>;
  if (q.error) return <Page title="Skills"><ErrorState error={q.error} retry={() => q.refetch()} /></Page>;
  if (!r) {
    return (
      <Page title="Not found">
        <Card><EmptyState title="That book or course isn't in your library" action={<Button onClick={() => navigate("/skills")}>Back to Skills</Button>} /></Card>
      </Page>
    );
  }
  return <Resource key={r.id} r={r} />;
}

function Resource({ r }: { r: SkillResource }) {
  const user = useUser();
  const tz = user.timezone;
  const navigate = useNavigate();
  const goals = useGoals();
  const sections = useSections(r.kind === "course" ? r.id : null);
  const sessions = useSessions(r.id);
  const [editing, setEditing] = useState(false);
  const [logging, setLogging] = useState<SkillSession | "new" | null>(null);
  const goal = r.goal_id ? goals.data?.find((g) => g.id === r.goal_id) : undefined;
  const Icon = r.kind === "book" ? BookOpen : MonitorPlay;
  const v = progressOf(r);
  const secs = useMemo(() => [...(sections.data ?? [])].sort((a, b) => a.ordinal - b.ordinal), [sections.data]);
  const sess = useMemo(() => [...(sessions.data ?? [])].sort((a, b) => b.started_at.localeCompare(a.started_at)), [sessions.data]);
  const studied = sess.reduce((m, s) => m + Math.max(0, (Date.parse(s.ended_at) - Date.parse(s.started_at)) / 60_000), 0);

  return (
    <Page
      wide
      title={<span className="flex items-center gap-3"><Link to="/skills" aria-label="Back to Skills" className="rounded-lg p-1 text-text-3 hover:bg-surface-2 hover:text-text"><ArrowLeft className="size-5" /></Link>{r.title}</span>}
      subtitle={<span className="flex flex-wrap items-center gap-2"><Icon className="size-4" aria-hidden />{r.kind === "book" ? "Book" : "Course"}{r.author ? ` · ${r.author}` : ""}<OriginBadge row={r} /></span>}
      actions={
        <>
          <Button icon={<PencilLine className="size-4" />} onClick={() => setEditing(true)}>Edit</Button>
          <Button variant="primary" icon={<Timer className="size-4" />} onClick={() => setLogging("new")}>Log session</Button>
        </>
      }
    >
      <div className="grid gap-5 lg:grid-cols-[minmax(0,4fr)_minmax(0,7fr)]">
        <div className="space-y-5">
          <Card className="relative overflow-hidden p-6">
            <div className="pointer-events-none absolute -right-16 -top-16 size-56 rounded-full bg-[var(--color-area-skill)] opacity-10 blur-3xl" aria-hidden />
            <div className="flex items-center gap-6">
              <Ring value={v} size={128} stroke={10} color="var(--color-area-skill)" label={`${Math.round(v * 100)}% done`}>
                <span className="font-display text-3xl font-semibold">{Math.round(v * 100)}%</span>
              </Ring>
              <div className="space-y-2 text-sm">
                <Badge tone={r.status === "done" ? "good" : r.status === "active" || !r.status ? "accent" : "neutral"} className="capitalize">{r.status ?? "active"}</Badge>
                <p className="text-text-2">{progressText(r)}</p>
                <p className="text-text-3">{sess.length} session{sess.length === 1 ? "" : "s"} · {minutesLabel(Math.round(studied))} studied</p>
              </div>
            </div>
            <div className="mt-5 flex flex-wrap gap-2 border-t border-line pt-4 text-[13px]">
              {goal ? <Link to={`/plan?goal=${goal.id}`} className="flex items-center gap-1.5 rounded-lg border border-line px-2.5 py-1 text-text-2 hover:border-accent/50 hover:text-text"><Flag className="size-3.5 text-accent" aria-hidden />{goal.title}</Link> : null}
              {r.url ? <a href={r.url} target="_blank" rel="noreferrer noopener" className="flex items-center gap-1.5 rounded-lg border border-line px-2.5 py-1 text-text-2 hover:border-accent/50 hover:text-text"><ExternalLink className="size-3.5" aria-hidden />Open</a> : null}
            </div>
          </Card>
          {r.kind === "course" ? <Sections r={r} sections={secs} sessions={sess} loading={sections.isPending} /> : null}
        </div>
        <Card>
          <CardHeader title="Sessions" icon={<Timer className="size-4" />} subtitle="Newest first" action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setLogging("new")}>Log</Button>} />
          {sessions.isPending ? <div className="px-5 pb-5"><Skeleton className="h-24" /></div> : !sess.length ? (
            <EmptyState className="py-10" title="No sessions yet" body="Log what you read or watched. A session also counts as skill time for the day." />
          ) : (
            <ul className="divide-y divide-line px-2 pb-2">
              {sess.map((s) => {
                const mins = Math.round((Date.parse(s.ended_at) - Date.parse(s.started_at)) / 60_000);
                const sec = s.section_id ? secs.find((x) => x.id === s.section_id) : undefined;
                return (
                  <li key={s.id}>
                    <button onClick={() => setLogging(s)} className="flex w-full items-center gap-4 rounded-xl px-3 py-3 text-left hover:bg-surface-2">
                      <span className="w-28 shrink-0 text-[12.5px] text-text-2">{fmt(s.started_at, tz, "EEE d MMM")}<span className="block font-mono text-text-3">{fmt(s.started_at, tz, "HH:mm")}–{fmt(s.ended_at, tz, "HH:mm")}</span></span>
                      <span className="min-w-0 flex-1">
                        <span className="block text-sm text-text">
                          {r.kind === "book" ? (s.page_from != null && s.page_to != null ? `Pages ${s.page_from}–${s.page_to}` : "Reading") : sec ? sec.title : "Watching"}
                        </span>
                        {s.notes ? <span className="block truncate text-[12.5px] text-text-3">{s.notes}</span> : null}
                      </span>
                      <span className="shrink-0 font-mono text-[12.5px] text-text-2">{minutesLabel(mins)}</span>
                      <OriginBadge row={s} />
                    </button>
                  </li>
                );
              })}
            </ul>
          )}
        </Card>
      </div>
      {editing ? <ResourceSheet open existing={r} onClose={() => setEditing(false)} onDeleted={() => navigate("/skills")} /> : null}
      {logging ? <SessionSheet key={logging === "new" ? "new" : logging.id} r={r} sections={secs} existing={logging === "new" ? undefined : logging} tz={tz} onClose={() => setLogging(null)} /> : null}
    </Page>
  );
}

function Sections({ r, sections, sessions, loading }: { r: SkillResource; sections: CourseSection[]; sessions: SkillSession[]; loading: boolean }) {
  const save = useSaveSection();
  const del = useDeleteSection();
  const saveResource = useSaveResource();
  const sumOf = (list: CourseSection[]) => list.reduce((m, x) => m + (x.duration_s ?? 0), 0);
  // The course's length follows its sections unless you set one yourself (progress is position ÷ length).
  const syncTotal = (next: number) => {
    const before = sumOf(sections);
    if (r.total_duration_s && r.total_duration_s !== before) return;
    if (next === (r.total_duration_s ?? 0)) return;
    saveResource.mutate({ existing: r, body: { kind: r.kind, title: r.title, total_duration_s: next || null } });
  };
  const [title, setTitle] = useState("");
  const [mins, setMins] = useState("");
  const done = new Set(sessions.map((s) => s.section_id).filter(Boolean));
  const total = sections.reduce((m, s) => m + (s.duration_s ?? 0), 0);
  // Ordinals are unique per course: move one out of the way first, and carry the version each write returns.
  const swap = async (i: number, j: number) => {
    const a = sections[i]!, b = sections[j]!;
    const parked = Math.max(...sections.map((x) => x.ordinal)) + 1;
    const body = (x: CourseSection, ordinal: number) => ({ resource_id: r.id, ordinal, title: x.title, duration_s: x.duration_s });
    try {
      const a1 = (await save.mutateAsync({ existing: a, body: body(a, parked) })) as CourseSection;
      await save.mutateAsync({ existing: b, body: body(b, a.ordinal) });
      await save.mutateAsync({ existing: a1, body: body(a, b.ordinal) });
    } catch (e) {
      toast.error(errText(e, "Couldn't reorder"));
    }
  };
  return (
    <Card>
      <CardHeader title="Sections" icon={<MonitorPlay className="size-4" />} subtitle={sections.length ? `${done.size} of ${sections.length} started${total ? ` · ${minutesLabel(Math.round(total / 60))}` : ""}` : "Break the course into its lessons"} />
      {loading ? <div className="px-5 pb-5"><Skeleton className="h-20" /></div> : (
        <ol className="space-y-1 px-3" aria-label="Sections">
          {sections.map((s, i) => (
            <li key={s.id} className="group flex items-center gap-2 rounded-lg px-2 py-1.5 hover:bg-surface-2/60">
              {done.has(s.id) ? <CheckCircle2 className="size-4 shrink-0 text-good" aria-label="Started" /> : <Circle className="size-4 shrink-0 text-text-3" aria-label="Not started" />}
              <span className="w-6 font-mono text-[11.5px] text-text-3">{i + 1}</span>
              <span className="min-w-0 flex-1 truncate text-[13.5px] text-text">{s.title}</span>
              {s.duration_s ? <span className="font-mono text-[12px] text-text-3">{minutesLabel(Math.round(s.duration_s / 60))}</span> : null}
              <span className="flex opacity-100 sm:opacity-0 sm:group-hover:opacity-100 sm:group-focus-within:opacity-100">
                <IconButton size="sm" label={`Move ${s.title} up`} disabled={i === 0 || save.isPending} onClick={() => swap(i, i - 1)}><ArrowUp className="size-3.5" /></IconButton>
                <IconButton size="sm" label={`Move ${s.title} down`} disabled={i === sections.length - 1 || save.isPending} onClick={() => swap(i, i + 1)}><ArrowDown className="size-3.5" /></IconButton>
                <IconButton size="sm" label={`Remove ${s.title}`} onClick={() => del.mutate(s, { onSuccess: () => syncTotal(sumOf(sections) - (s.duration_s ?? 0)), onError: (e) => toast.error(errText(e, "Couldn't remove")) })}><Trash2 className="size-3.5" /></IconButton>
              </span>
            </li>
          ))}
        </ol>
      )}
      <form
        className="mt-2 flex items-end gap-2 border-t border-line px-5 py-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (!title.trim()) return;
          const ordinal = (sections[sections.length - 1]?.ordinal ?? 0) + 1;
          const dur = mins ? Math.round(Number(mins) * 60) : null;
          save.mutate({ body: { resource_id: r.id, ordinal, title: title.trim(), duration_s: dur } },
            { onSuccess: () => { setTitle(""); setMins(""); syncTotal(sumOf(sections) + (dur ?? 0)); }, onError: (err) => toast.error(errText(err)) });
        }}
      >
        <Field label="New section" className="flex-1">{(f) => <Input {...f} value={title} maxLength={200} onChange={(e) => setTitle(e.target.value)} placeholder="Atomics and memory order" />}</Field>
        <Field label="Minutes" className="w-24">{(f) => <Input {...f} type="number" min={1} value={mins} onChange={(e) => setMins(e.target.value)} />}</Field>
        <Button type="submit" icon={<Plus className="size-4" />} disabled={!title.trim()} loading={save.isPending && !save.variables?.existing}>Add</Button>
      </form>
    </Card>
  );
}

function SessionSheet({ r, sections, existing, tz, onClose }: { r: SkillResource; sections: CourseSection[]; existing?: SkillSession; tz: string; onClose: () => void }) {
  const now = useNow();
  const s = existing;
  const save = useSaveSession();
  const del = useDeleteSession();
  const [start, setStart] = useState(toLocalInput(s?.started_at ?? now - 45 * 60_000, tz));
  const [end, setEnd] = useState(toLocalInput(s?.ended_at ?? now, tz));
  // Pages are 1-based: a new session starts on the page after the last one read.
  const nextPage = Math.max(1, (r.progress?.pages_done ?? 0) + 1);
  const [from, setFrom] = useState(String(s?.page_from ?? nextPage));
  const [to, setTo] = useState(s?.page_to != null ? String(s.page_to) : "");
  const [sectionId, setSectionId] = useState(s?.section_id ?? "");
  const [posFrom, setPosFrom] = useState(s?.pos_from_s != null ? String(Math.round(s.pos_from_s / 60)) : "");
  const [posTo, setPosTo] = useState(s?.pos_to_s != null ? String(Math.round(s.pos_to_s / 60)) : "");
  const [notes, setNotes] = useState(s?.notes ?? "");
  const si = fromLocalInput(start, tz);
  const ei = fromLocalInput(end, tz);
  const badRange = !si || !ei || Date.parse(ei) <= Date.parse(si);
  const badFrom = r.kind === "book" && from !== "" && Number(from) < 1;
  const badPages = r.kind === "book" && to !== "" && Number(to) < Number(from);
  const valid = !badRange && !badPages && !badFrom;
  return (
    <Sheet
      open
      onOpenChange={(o) => !o && onClose()}
      title={s ? "Edit session" : "Log session"}
      description={r.title}
      footer={
        <>
          {s ? <DeleteButton loading={del.isPending} onDelete={() => del.mutate(s, { onSuccess: () => { toast("Session deleted"); onClose(); }, onError: (e) => toast.error(errText(e, "Couldn't delete")) })} /> : null}
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button type="submit" form="session-form" variant="primary" disabled={!valid} loading={save.isPending}>{s ? "Save" : "Log it"}</Button>
        </>
      }
    >
      <form
        id="session-form"
        className="space-y-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (!valid) return;
          save.mutate(
            {
              existing: s,
              body: {
                resource_id: r.id, started_at: si!, ended_at: ei!, notes: notes.trim() || null,
                ...(r.kind === "book"
                  ? { page_from: from === "" ? null : Number(from), page_to: to === "" ? null : Number(to) }
                  : { section_id: sectionId || null, pos_from_s: posFrom === "" ? null : Number(posFrom) * 60, pos_to_s: posTo === "" ? null : Number(posTo) * 60 }),
                ...(s ? {} : { source: "manual" as const }),
              },
            },
            { onSuccess: () => { toast.success(s ? "Saved" : "Session logged"); onClose(); }, onError: (err) => toast.error(errText(err)) },
          );
        }}
      >
        <div className="grid grid-cols-2 gap-3">
          <Field label="From">{(f) => <Input {...f} type="datetime-local" value={start} onChange={(e) => setStart(e.target.value)} />}</Field>
          <Field label="To" error={badRange ? "Ends before it starts" : undefined}>{(f) => <Input {...f} type="datetime-local" value={end} onChange={(e) => setEnd(e.target.value)} />}</Field>
        </div>
        {r.kind === "book" ? (
          <div className="grid grid-cols-2 gap-3">
            <Field label="From page" error={badFrom ? "Pages start at 1" : undefined}>{(f) => <Input {...f} type="number" min={1} value={from} onChange={(e) => setFrom(e.target.value)} />}</Field>
            <Field label="To page" error={badPages ? "Before the first page" : undefined} hint={r.total_pages ? `of ${r.total_pages}` : undefined}>
              {(f) => <Input {...f} type="number" min={1} max={r.total_pages ?? undefined} value={to} onChange={(e) => setTo(e.target.value)} />}
            </Field>
          </div>
        ) : (
          <>
            <Field label="Section">
              {(f) => (
                <NativeSelect {...f} value={sectionId} onChange={(e) => setSectionId(e.target.value)}>
                  <option value="">Not a specific section</option>
                  {sections.map((x, i) => <option key={x.id} value={x.id}>{i + 1}. {x.title}</option>)}
                </NativeSelect>
              )}
            </Field>
            <div className="grid grid-cols-2 gap-3">
              <Field label="From minute">{(f) => <Input {...f} type="number" min={0} value={posFrom} onChange={(e) => setPosFrom(e.target.value)} />}</Field>
              <Field label="To minute">{(f) => <Input {...f} type="number" min={0} value={posTo} onChange={(e) => setPosTo(e.target.value)} />}</Field>
            </div>
          </>
        )}
        <Field label="Notes">{(f) => <Textarea {...f} rows={3} value={notes} onChange={(e) => setNotes(e.target.value)} placeholder="What stuck" />}</Field>
        {save.error ? <p role="alert" className={cn("text-[13px] text-alert-text")}>{errText(save.error)}</p> : null}
      </form>
    </Sheet>
  );
}
