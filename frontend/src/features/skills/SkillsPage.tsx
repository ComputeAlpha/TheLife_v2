import { useState } from "react";
import { Link } from "react-router";
import { BookOpen, GraduationCap, MonitorPlay, Plus } from "lucide-react";
import { useResources } from "@/api/skills";
import type { SkillResource } from "@/api/types";
import { minutesLabel } from "@/lib/time";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Ring } from "@/ui/Ring";
import { Segmented } from "@/ui/Segmented";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { Page } from "../common/Page";
import { ResourceSheet } from "./ResourceSheet";

type Show = "current" | "planned" | "finished" | "all";
const SHOW: Record<Show, (r: SkillResource) => boolean> = {
  current: (r) => r.status === "active" || r.status === "paused" || !r.status,
  planned: (r) => r.status === "planned",
  finished: (r) => r.status === "done" || r.status === "dropped",
  all: () => true,
};
const STATUS_TONE = { active: "accent", paused: "warn", planned: "neutral", done: "good", dropped: "neutral" } as const;

/** 0..1 from the server's progress (percent when present, else derived). */
export function progressOf(r: SkillResource): number {
  const p = r.progress;
  if (p?.percent != null) return p.percent / 100;
  if (r.kind === "book" && r.total_pages && p?.pages_done != null) return p.pages_done / r.total_pages;
  if (r.kind === "course" && r.total_duration_s && p?.position_s != null) return p.position_s / r.total_duration_s;
  return 0;
}

export function progressText(r: SkillResource): string {
  const p = r.progress;
  if (r.kind === "book") return r.total_pages ? `${p?.pages_done ?? 0} of ${r.total_pages} pages` : `${p?.pages_done ?? 0} pages`;
  const pos = Math.round((p?.position_s ?? 0) / 60);
  return r.total_duration_s ? `${minutesLabel(pos)} of ${minutesLabel(Math.round(r.total_duration_s / 60))}` : minutesLabel(pos);
}

export function SkillsPage() {
  const q = useResources();
  const [show, setShow] = useState<Show>("current");
  const [creating, setCreating] = useState(false);
  const items = (q.data ?? []).filter(SHOW[show]).sort((a, b) => progressOf(b) - progressOf(a) || a.title.localeCompare(b.title));
  return (
    <Page
      wide
      title="Skills"
      subtitle="Books and courses you're working through. Log a session and progress, targets and scores follow."
      actions={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setCreating(true)}>Add book or course</Button>}
    >
      <Segmented<Show> className="mb-5" label="Which resources" value={show} onChange={setShow}
        options={[{ value: "current", label: "In progress" }, { value: "planned", label: "Planned" }, { value: "finished", label: "Finished" }, { value: "all", label: "All" }]} />
      {q.isPending ? (
        <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-3">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-40" />)}</div>
      ) : q.error ? (
        <ErrorState error={q.error} retry={() => q.refetch()} />
      ) : !items.length ? (
        <Card>
          <EmptyState art={<GraduationCap className="size-10 text-accent" />} title={q.data?.length ? "Nothing here" : "Your library is empty"}
            body="Add the book or course you're on. The assistant plans study blocks around it and your skill score counts the sessions."
            action={<Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setCreating(true)}>Add book or course</Button>} />
        </Card>
      ) : (
        <ul className="grid gap-4 sm:grid-cols-2 xl:grid-cols-3">
          {items.map((r) => {
            const Icon = r.kind === "book" ? BookOpen : MonitorPlay;
            const v = progressOf(r);
            return (
              <li key={r.id}>
                <Link to={`/skills/${r.id}`} className="group block h-full">
                  <Card className="relative flex h-full items-center gap-5 overflow-hidden p-5 transition-colors group-hover:border-line-strong">
                    <div className="pointer-events-none absolute -right-10 -top-10 size-32 rounded-full bg-[var(--color-area-skill)] opacity-[.07] blur-2xl" aria-hidden />
                    <Ring value={v} size={76} stroke={7} color="var(--color-area-skill)" label={`${Math.round(v * 100)}% done`}>
                      <span className="font-mono text-[15px] font-semibold">{Math.round(v * 100)}%</span>
                    </Ring>
                    <div className="min-w-0 flex-1">
                      <p className="flex items-center gap-1.5 text-[11.5px] font-semibold uppercase tracking-wider text-text-3"><Icon className="size-3.5" aria-hidden />{r.kind}</p>
                      <p className="mt-1 line-clamp-2 text-[15px] font-semibold leading-snug text-text group-hover:text-accent">{r.title}</p>
                      {r.author ? <p className="truncate text-[12.5px] text-text-2">{r.author}</p> : null}
                      <div className="mt-2 flex flex-wrap items-center gap-2">
                        <Badge tone={STATUS_TONE[r.status ?? "active"]} className="capitalize">{r.status ?? "active"}</Badge>
                        <span className="text-[12px] text-text-3">{progressText(r)}</span>
                      </div>
                    </div>
                  </Card>
                </Link>
              </li>
            );
          })}
        </ul>
      )}
      {creating ? <ResourceSheet open onClose={() => setCreating(false)} /> : null}
    </Page>
  );
}
