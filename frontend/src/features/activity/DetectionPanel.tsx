import { useState } from "react";
import { ArrowRight, Check, CircleAlert, History, PencilLine, ThumbsDown, X } from "lucide-react";
import type { Area, Detection } from "@/api/types";
import { detectionSource } from "@/api/types";
import { useConfirmDetections, useCorrectDetection, useDetectionHistory, useDisputeDetection } from "@/api/review";
import { areaMeta } from "@/lib/areas";
import { fmt, fromLocalInput, minutesLabel, relative, timeRange, toLocalInput } from "@/lib/time";
import { AreaPicker } from "@/ui/AreaPicker";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Field, Input, Textarea } from "@/ui/Field";
import { Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { ApiError } from "@/api/errors";
import { areaVars } from "../calendar/EntryChip";
import { ConfidenceMeter, SOURCE_META, STATUS_META } from "./meta";
import type { ReviewStatus } from "@/lib/activity/review";

type Mode = "view" | "dispute" | "correct";

export function DetectionPanel({ d, tz, onSelectId, onClose }: { d: Detection; tz: string; onSelectId: (id: string) => void; onClose?: () => void }) {
  const [mode, setMode] = useState<Mode>("view");
  const status = (d.status ?? "detected") as ReviewStatus;
  const a = areaMeta(d.area);
  const src = SOURCE_META[detectionSource(d)];
  const confirm = useConfirmDetections();
  const mins = Math.round((Date.parse(d.ended_at) - Date.parse(d.started_at)) / 60_000);
  const evidence = Object.entries((d.evidence ?? {}) as Record<string, unknown>).filter(([, v]) => v != null && typeof v !== "object");

  return (
    <div className="flex flex-col gap-5" style={areaVars(d.area)}>
      <div className="flex items-start gap-3">
        <span className="grid size-10 shrink-0 place-items-center rounded-xl bg-[color-mix(in_oklab,var(--a)_16%,transparent)] text-[var(--a)]">
          <a.icon className="size-5" aria-hidden />
        </span>
        <div className="min-w-0 flex-1">
          <h2 className="text-[15px] font-semibold leading-snug text-text">{d.summary}</h2>
          <p className="mt-0.5 text-[12.5px] text-text-3">{timeRange(d.started_at, d.ended_at, tz)} · {minutesLabel(mins)}</p>
        </div>
        <Badge tone={STATUS_META[status].tone}>{STATUS_META[status].label}</Badge>
        {onClose ? (
          <button onClick={onClose} aria-label="Close details" className="rounded-md p-1 text-text-3 hover:bg-surface-2 hover:text-text">
            <X className="size-4" />
          </button>
        ) : null}
      </div>

      <dl className="grid grid-cols-2 gap-x-4 gap-y-3 rounded-xl border border-line bg-surface-2/40 p-4 text-[13px]">
        <Fact label="Area"><span className="inline-flex items-center gap-1.5"><a.icon className="size-3.5 text-[var(--a)]" aria-hidden />{a.label}</span></Fact>
        <Fact label="Category">{d.category}</Fact>
        <Fact label="Source"><span className="inline-flex items-center gap-1.5" title={src.hint}><src.icon className="size-3.5 text-text-3" aria-hidden />{src.label}</span></Fact>
        <Fact label="Confidence">
          <span className="inline-flex items-center gap-2">
            <ConfidenceMeter value={d.confidence} />
            <span className="font-mono text-[12px]">{Math.round((d.confidence ?? 1) * 100)}%</span>
          </span>
        </Fact>
        {evidence.slice(0, 4).map(([k, v]) => (
          <Fact key={k} label={k.replace(/_/g, " ")} wide><span className="break-words font-mono text-[12px] text-text-2">{String(v)}</span></Fact>
        ))}
      </dl>

      {status === "disputed" ? (
        <Callout tone="warn" icon={<CircleAlert className="size-4" />}>
          You disputed this. The assistant takes another look and corrects it or asks you; it stays out of your scores meanwhile.
        </Callout>
      ) : null}
      {status === "superseded" && d.superseded_by_id ? (
        <Callout tone="neutral" icon={<PencilLine className="size-4" />}>
          Replaced by a correction.{" "}
          <button className="text-accent hover:underline" onClick={() => onSelectId(d.superseded_by_id!)}>Show the correction</button>
        </Callout>
      ) : null}

      {status !== "superseded" && mode === "view" ? (
        <div className="flex flex-wrap gap-2">
          {status !== "confirmed" ? (
            <Button variant="primary" icon={<Check className="size-4" />} loading={confirm.isPending}
              onClick={() => confirm.mutate([d.id], { onSuccess: () => toast.success("Confirmed"), onError: (e) => toast.error(errText(e)) })}>
              Confirm
            </Button>
          ) : null}
          {status === "detected" ? (
            <Button icon={<ThumbsDown className="size-4" />} onClick={() => setMode("dispute")}>Dispute</Button>
          ) : null}
          <Button icon={<PencilLine className="size-4" />} onClick={() => setMode("correct")}>Correct</Button>
        </div>
      ) : null}
      {mode === "dispute" ? <DisputeForm d={d} onDone={() => setMode("view")} /> : null}
      {mode === "correct" ? <CorrectForm d={d} tz={tz} onDone={(id) => { setMode("view"); if (id) onSelectId(id); }} /> : null}

      <HistoryBlock d={d} tz={tz} onSelectId={onSelectId} />
    </div>
  );
}

function Fact({ label, children, wide }: { label: string; children: React.ReactNode; wide?: boolean }) {
  return (
    <div className={cn("min-w-0", wide && "col-span-2")}>
      <dt className="text-[11px] font-semibold uppercase tracking-wider text-text-3">{label}</dt>
      <dd className="mt-0.5 text-text">{children}</dd>
    </div>
  );
}

function Callout({ tone, icon, children }: { tone: "warn" | "neutral"; icon: React.ReactNode; children: React.ReactNode }) {
  return (
    <div className={cn("flex gap-2.5 rounded-xl border p-3 text-[13px]", tone === "warn" ? "border-warn/30 bg-warn/[.06] text-text-2" : "border-line bg-surface-2/50 text-text-2")}>
      <span className={cn("mt-0.5 shrink-0", tone === "warn" ? "text-warn" : "text-text-3")}>{icon}</span>
      <p>{children}</p>
    </div>
  );
}

function errText(e: unknown) {
  return e instanceof ApiError ? e.detail ?? e.title : "Something went wrong";
}

function DisputeForm({ d, onDone }: { d: Detection; onDone: () => void }) {
  const [reason, setReason] = useState("");
  const dispute = useDisputeDetection();
  return (
    <form
      className="space-y-3 rounded-xl border border-line p-4"
      onSubmit={(e) => {
        e.preventDefault();
        dispute.mutate({ id: d.id, reason }, {
          onSuccess: () => { toast("Disputed: the assistant will take another look"); onDone(); },
          onError: (err) => toast.error(errText(err)),
        });
      }}
    >
      <Field label="What was wrong? (optional)" hint="The assistant uses this when it looks again.">
        {(f) => <Textarea {...f} rows={2} value={reason} onChange={(e) => setReason(e.target.value)} placeholder="I was reading docs for the release, not browsing" autoFocus />}
      </Field>
      <div className="flex justify-end gap-2">
        <Button variant="ghost" onClick={onDone}>Cancel</Button>
        <Button type="submit" variant="danger" icon={<ThumbsDown className="size-4" />} loading={dispute.isPending}>Dispute</Button>
      </div>
    </form>
  );
}

function CorrectForm({ d, tz, onDone }: { d: Detection; tz: string; onDone: (newId?: string) => void }) {
  const [area, setArea] = useState<Area>(d.area);
  const [category, setCategory] = useState(d.category);
  const [summary, setSummary] = useState(d.summary);
  const [start, setStart] = useState(toLocalInput(d.started_at, tz));
  const [end, setEnd] = useState(toLocalInput(d.ended_at, tz));
  const correct = useCorrectDetection();
  const s = fromLocalInput(start, tz);
  const e = fromLocalInput(end, tz);
  const badRange = !s || !e || Date.parse(e) <= Date.parse(s);
  return (
    <form
      className="space-y-4 rounded-xl border border-accent/30 bg-accent/[.03] p-4"
      onSubmit={(ev) => {
        ev.preventDefault();
        if (badRange) return;
        correct.mutate(
          { id: d.id, body: { area, category: category.trim() || "general", summary: summary.trim(), started_at: s!, ended_at: e! } },
          {
            onSuccess: (res) => {
              toast.success("Corrected");
              onDone("id" in res ? res.id : undefined);
            },
            onError: (err) => toast.error(errText(err)),
          },
        );
      }}
    >
      <p className="text-[13px] text-text-2">Say what you were really doing. This replaces the detection and counts as confirmed.</p>
      <AreaPicker value={area} onChange={setArea} />
      <Field label="What you were doing">
        {(f) => <Input {...f} required maxLength={500} value={summary} onChange={(ev) => setSummary(ev.target.value)} />}
      </Field>
      <Field label="Category" hint="A short label, like coding, reading or gym.">
        {(f) => <Input {...f} maxLength={80} value={category} onChange={(ev) => setCategory(ev.target.value)} />}
      </Field>
      <div className="grid grid-cols-2 gap-3">
        <Field label="From">{(f) => <Input {...f} type="datetime-local" value={start} onChange={(ev) => setStart(ev.target.value)} />}</Field>
        <Field label="To" error={badRange ? "Ends before it starts" : undefined}>{(f) => <Input {...f} type="datetime-local" value={end} onChange={(ev) => setEnd(ev.target.value)} />}</Field>
      </div>
      <div className="flex justify-end gap-2">
        <Button variant="ghost" onClick={() => onDone()}>Cancel</Button>
        <Button type="submit" variant="primary" icon={<Check className="size-4" />} loading={correct.isPending} disabled={badRange || !summary.trim()}>Save correction</Button>
      </div>
    </form>
  );
}

const ACTOR: Record<string, string> = { user: "You", agent: "Assistant", device: "Collector", system: "TheLife", external: "External" };

function HistoryBlock({ d, tz, onSelectId }: { d: Detection; tz: string; onSelectId: (id: string) => void }) {
  const h = useDetectionHistory(d.id);
  const events = h.data?.events ?? [];
  const chain = h.data?.chain ?? [];
  return (
    <section aria-label="History" className="border-t border-line pt-4">
      <h3 className="mb-3 flex items-center gap-2 text-[12px] font-semibold uppercase tracking-wider text-text-3"><History className="size-3.5" /> History</h3>
      {h.isPending ? <Skeleton className="h-16" /> : null}
      <ol className="relative space-y-3 border-l border-line pl-4">
        <li className="relative">
          <span className="absolute -left-[21px] top-1.5 size-2 rounded-full bg-text-3" aria-hidden />
          <p className="text-[13px] text-text-2"><span className="text-text">{ACTOR[d.created_by.kind] ?? d.created_by.kind}</span> recorded it</p>
          <p className="text-[11.5px] text-text-3">{fmt(d.created_at, tz, "d MMM, HH:mm")} · {relative(d.created_at)}</p>
        </li>
        {events.map((ev) => (
          <li key={ev.id} className="relative">
            <span className={cn("absolute -left-[21px] top-1.5 size-2 rounded-full", ev.to_status === "confirmed" ? "bg-good" : ev.to_status === "disputed" ? "bg-warn" : "bg-accent")} aria-hidden />
            <p className="flex flex-wrap items-center gap-1.5 text-[13px] text-text-2">
              <span className="text-text">{ACTOR[ev.actor_kind] ?? ev.actor_kind}</span>
              <span className="capitalize">{ev.from_status}</span>
              <ArrowRight className="size-3 text-text-3" aria-label="to" />
              <span className="capitalize text-text">{ev.to_status}</span>
            </p>
            {ev.reason ? <p className="mt-0.5 text-[12.5px] italic text-text-2">“{ev.reason}”</p> : null}
            <p className="text-[11.5px] text-text-3">{fmt(ev.at, tz, "d MMM, HH:mm")}</p>
          </li>
        ))}
      </ol>
      {chain.length > 1 ? (
        <div className="mt-4">
          <p className="mb-2 text-[12px] text-text-3">Correction chain</p>
          <ol className="space-y-1.5">
            {chain.map((c, i) => {
              const ca = areaMeta(c.area);
              return (
                <li key={c.id} style={areaVars(c.area)}>
                  <button
                    onClick={() => onSelectId(c.id)}
                    disabled={c.id === d.id}
                    className={cn("flex w-full items-center gap-2 rounded-lg border px-2.5 py-1.5 text-left text-[12.5px]",
                      c.id === d.id ? "border-accent/40 bg-accent/[.06]" : "border-line hover:border-line-strong")}
                  >
                    <span className="font-mono text-[11px] text-text-3">{i + 1}</span>
                    <ca.icon className="size-3.5 shrink-0 text-[var(--a)]" aria-hidden />
                    <span className={cn("min-w-0 flex-1 truncate", c.status === "superseded" && "text-text-3 line-through")}>{c.summary}</span>
                    <span className="shrink-0 text-[11px] text-text-3">{STATUS_META[(c.status ?? "detected") as ReviewStatus].label}</span>
                  </button>
                </li>
              );
            })}
          </ol>
        </div>
      ) : null}
    </section>
  );
}
