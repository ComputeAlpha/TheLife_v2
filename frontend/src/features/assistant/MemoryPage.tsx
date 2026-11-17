import { useMemo, useState } from "react";
import { Brain, ChevronRight, History, Search, Trash2 } from "lucide-react";
import { useUser } from "@/api/auth";
import { useForgetFact, useMemoryFacts } from "@/api/assistant";
import type { MemoryFact } from "@/api/types";
import { fmt, relative } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Dialog } from "@/ui/Dialog";
import { Input } from "@/ui/Field";
import { Segmented } from "@/ui/Segmented";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { ConfidenceMeter } from "../activity/meta";
import { errText } from "../plan/shared";

type Show = "active" | "proposed" | "all";
const TONE = { active: "accent", proposed: "warn", superseded: "neutral" } as const;

/** Superseded versions of a fact, newest first, by following superseded_by back from it. */
export function olderVersions(fact: MemoryFact, all: MemoryFact[]): MemoryFact[] {
  const out: MemoryFact[] = [];
  let cur = fact.id;
  for (let guard = 0; guard < 50; guard++) {
    const prev = all.find((f) => f.superseded_by === cur);
    if (!prev) break;
    out.push(prev);
    cur = prev.id;
  }
  return out;
}

export function MemoryPage() {
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const [show, setShow] = useState<Show>("active");
  const [q, setQ] = useState("");
  const all = useMemoryFacts(undefined);
  const [open, setOpen] = useState<Set<string>>(new Set());
  const [forgetting, setForgetting] = useState<MemoryFact | null>(null);
  const forget = useForgetFact();
  const facts = useMemo(() => all.data ?? [], [all.data]);
  const shown = useMemo(() => facts
    .filter((f) => (show === "all" ? f.status !== "superseded" || !!q : (f.status ?? "active") === show))
    .filter((f) => !q.trim() || `${f.statement ?? ""} ${f.predicate ?? ""} ${f.value ?? ""}`.toLowerCase().includes(q.trim().toLowerCase()))
    .sort((a, b) => (b.last_used_at ?? b.updated_at).localeCompare(a.last_used_at ?? a.updated_at)), [facts, show, q]);
  const proposed = facts.filter((f) => f.status === "proposed").length;

  return (
    <>
      <div className="mb-4 flex flex-wrap items-center gap-3">
        <Segmented<Show> label="Which facts" value={show} onChange={setShow}
          options={[{ value: "active", label: "Remembered" }, { value: "proposed", label: `Proposed${proposed ? ` · ${proposed}` : ""}` }, { value: "all", label: "All" }]} />
        <div className="relative ml-auto">
          <Search className="pointer-events-none absolute left-2.5 top-1/2 size-4 -translate-y-1/2 text-text-3" aria-hidden />
          <Input aria-label="Search memory" className="h-9 w-64 pl-8 text-[13px]" value={q} onChange={(e) => setQ(e.target.value)} placeholder="Search what it remembers" />
        </div>
      </div>
      <p className="mb-4 max-w-3xl text-[13px] text-text-3">
        What the assistant has learned about you from chat. It uses these when it plans and replies. Forget anything that's wrong or private; it's deleted for good.
      </p>
      {all.isPending ? <Skeleton className="h-48" /> : all.error ? <ErrorState error={all.error} retry={() => all.refetch()} /> : !shown.length ? (
        <Card><EmptyState art={<Brain className="size-10 text-accent" />} title={facts.length ? "Nothing matches" : "Nothing remembered yet"} body={facts.length ? undefined : "As you chat, the assistant notes durable facts: your gym days, who Priya is, how you like your mornings."} /></Card>
      ) : (
        <Card className="p-2">
          <ul className="divide-y divide-line" aria-label="Facts">
            {shown.map((f) => {
              const older = olderVersions(f, facts);
              const expanded = open.has(f.id);
              return (
                <li key={f.id} className="px-3 py-3">
                  <div className="flex items-start gap-3">
                    <button
                      type="button"
                      className={cn("mt-0.5 grid size-6 shrink-0 place-items-center rounded-md text-text-3", older.length ? "hover:bg-surface-2 hover:text-text" : "invisible")}
                      aria-expanded={older.length ? expanded : undefined}
                      aria-label={`Earlier versions of: ${f.statement}`}
                      tabIndex={older.length ? 0 : -1}
                      onClick={() => setOpen((s) => { const x = new Set(s); if (x.has(f.id)) x.delete(f.id); else x.add(f.id); return x; })}
                    >
                      <ChevronRight className={cn("size-4 transition-transform", expanded && "rotate-90")} />
                    </button>
                    <div className="min-w-0 flex-1">
                      <p className={cn("text-sm", f.status === "superseded" ? "text-text-3 line-through" : "text-text")}>{f.statement ?? `${f.predicate}: ${f.value}`}</p>
                      <p className="mt-1 flex flex-wrap items-center gap-x-3 gap-y-1 text-[12px] text-text-3">
                        <span className="flex items-center gap-1.5"><ConfidenceMeter value={f.confidence} />{Math.round((f.confidence ?? 1) * 100)}%</span>
                        <Badge tone={TONE[f.status ?? "active"]} className="capitalize">{f.status ?? "active"}</Badge>
                        {f.valid_from ? <span title={fmt(f.valid_from, tz, "d MMM yyyy")}>since {fmt(f.valid_from, tz, "d MMM yyyy")}{f.valid_to ? ` until ${fmt(f.valid_to, tz, "d MMM yyyy")}` : ""}</span> : null}
                        <span>{f.last_used_at ? `used ${relative(f.last_used_at, now)}` : "not used yet"}</span>
                        {older.length ? <span className="flex items-center gap-1"><History className="size-3" aria-hidden />{older.length} earlier</span> : null}
                      </p>
                    </div>
                    <Button size="sm" variant="ghost" icon={<Trash2 className="size-3.5" />} onClick={() => setForgetting(f)} aria-label={`Forget: ${f.statement}`}>Forget</Button>
                  </div>
                  {expanded ? (
                    <ol className="ml-9 mt-2 space-y-1.5 border-l border-line pl-4" aria-label="Earlier versions">
                      {older.map((o) => (
                        <li key={o.id} className="text-[13px] text-text-3">
                          <span className="line-through">{o.statement}</span>
                          <span className="ml-2 text-[11.5px]">{o.valid_from ? fmt(o.valid_from, tz, "d MMM yyyy") : ""}</span>
                        </li>
                      ))}
                    </ol>
                  ) : null}
                </li>
              );
            })}
          </ul>
        </Card>
      )}
      <Dialog
        open={!!forgetting}
        onOpenChange={(o) => !o && setForgetting(null)}
        title="Forget this?"
        description="The assistant won't know it any more. This can't be undone."
        footer={
          <>
            <Button variant="ghost" onClick={() => setForgetting(null)}>Keep it</Button>
            <Button variant="danger" icon={<Trash2 className="size-4" />} loading={forget.isPending}
              onClick={() => forgetting && forget.mutate(forgetting, { onSuccess: () => { toast("Forgotten"); setForgetting(null); }, onError: (e) => toast.error(errText(e, "Couldn't forget it")) })}>Forget</Button>
          </>
        }
      >
        <p className="rounded-lg border border-line bg-surface-2/50 p-3 text-sm text-text">{forgetting?.statement}</p>
      </Dialog>
    </>
  );
}
