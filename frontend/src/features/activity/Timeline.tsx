import { useMemo } from "react";
import { Check } from "lucide-react";
import type { CalendarEntry, Detection } from "@/api/types";
import { detectionSource } from "@/api/types";
import { layoutColumn } from "@/lib/calendar/layout";
import { minutesInto, type Day } from "@/lib/calendar/days";
import { areaMeta } from "@/lib/areas";
import { fmt, timeRange } from "@/lib/time";
import { cn } from "@/ui/cn";
import { areaVars } from "../calendar/EntryChip";
import { ConfidenceMeter, SOURCE_META } from "./meta";

const PX_PER_MIN = 1.6; // 96 px per hour: a 15-minute detection is two text lines

interface Props {
  day: Day;
  tz: string;
  hours: [number, number];
  detections: Detection[];
  planned: CalendarEntry[];
  selectedId: string | null;
  onSelect: (d: Detection) => void;
  selecting: boolean;
  picked: Set<string>;
  onPick: (id: string) => void;
  nowMinute: number | null;
}

export function Timeline(p: Props) {
  const [h0, h1] = p.hours;
  const top = (m: number) => (m - h0 * 60) * PX_PER_MIN;
  const height = (h1 - h0) * 60 * PX_PER_MIN;

  const placed = useMemo(
    () =>
      layoutColumn(
        p.detections.map((d) => ({
          item: d,
          start: Math.max(h0 * 60, minutesInto(Date.parse(d.started_at), p.day, p.tz)),
          end: Math.min(h1 * 60, minutesInto(Date.parse(d.ended_at), p.day, p.tz)),
        })),
        15,
      ),
    [p.detections, p.day, p.tz, h0, h1],
  );
  const ghosts = useMemo(
    () =>
      layoutColumn(
        p.planned
          .filter((e) => !e.all_day && e.starts_at && e.ends_at && e.starts_at !== e.ends_at)
          .map((e) => ({
            item: e,
            start: Math.max(h0 * 60, minutesInto(Date.parse(e.starts_at!), p.day, p.tz)),
            end: Math.min(h1 * 60, minutesInto(Date.parse(e.ends_at!), p.day, p.tz)),
          })),
        15,
      ),
    [p.planned, p.day, p.tz, h0, h1],
  );

  return (
    <div className="relative rounded-2xl border border-line bg-surface-1">
      <div className="sticky top-16 z-10 flex border-b border-line bg-surface-1/95 text-[11px] font-semibold uppercase tracking-wider text-text-3 backdrop-blur">
        <div className="w-14 shrink-0" />
        <div className="w-[22%] max-w-44 border-l border-line px-2 py-2">Planned</div>
        <div className="flex-1 border-l border-line px-3 py-2">Detected</div>
      </div>
      <div className="relative flex" style={{ height }}>
        {/* Hour axis */}
        <div className="relative w-14 shrink-0" aria-hidden>
          {Array.from({ length: h1 - h0 + 1 }, (_, i) => (
            <span key={i} className="absolute right-2 -translate-y-1/2 font-mono text-[11px] text-text-3" style={{ top: i * 60 * PX_PER_MIN }}>
              {i === 0 ? "" : `${String(h0 + i).padStart(2, "0")}:00`}
            </span>
          ))}
        </div>
        <div className="pointer-events-none absolute inset-y-0 left-14 right-0" aria-hidden>
          {Array.from({ length: h1 - h0 }, (_, i) => (
            <div key={i} className="absolute inset-x-0 border-t border-line/70" style={{ top: i * 60 * PX_PER_MIN }}>
              <div className="absolute inset-x-0 top-[48px] border-t border-dashed border-line/35" />
            </div>
          ))}
        </div>

        {/* Planned lane: ghost outlines of what the calendar said */}
        <ol className="relative w-[22%] max-w-44 border-l border-line" aria-label="Planned">
          {ghosts.map((g) => {
            const e = g.item;
            const a = areaMeta(e.area);
            return (
              <li
                key={`${e.kind}:${e.id}:${e.starts_at}`}
                style={{ ...areaVars(e.area), top: top(g.start) + 1, height: Math.max(18, (g.end - g.start) * PX_PER_MIN - 2), left: `${(g.col / g.cols) * 100}%`, width: `calc(${(g.span / g.cols) * 100}% - 4px)` }}
                className="absolute ml-1 overflow-hidden rounded-md border border-dashed border-[color-mix(in_oklab,var(--a)_55%,transparent)] bg-[color-mix(in_oklab,var(--a)_5%,transparent)] px-1.5 py-0.5"
                title={`${e.title} · ${timeRange(e.starts_at!, e.ends_at!, p.tz)}`}
              >
                <p className="flex items-center gap-1 truncate text-[11.5px] text-text-2">
                  <a.icon className="size-3 shrink-0 text-[var(--a)]" aria-hidden />
                  <span className="truncate">{e.title}</span>
                </p>
              </li>
            );
          })}
        </ol>

        {/* Detections */}
        <ol className="relative flex-1 border-l border-line" aria-label="Detected">
          {placed.map((pl) => {
            const d = pl.item;
            const h = Math.max(22, (pl.end - pl.start) * PX_PER_MIN - 3);
            return (
              <li
                key={d.id}
                className="absolute px-[3px]"
                style={{ top: top(pl.start) + 1.5, height: h, left: `${(pl.col / pl.cols) * 100}%`, width: `${(pl.span / pl.cols) * 100}%` }}
              >
                <DetectionBlock
                  d={d}
                  tz={p.tz}
                  tall={h >= 44}
                  selected={p.selectedId === d.id}
                  selecting={p.selecting}
                  picked={p.picked.has(d.id)}
                  onClick={() => (p.selecting && d.status === "detected" ? p.onPick(d.id) : p.onSelect(d))}
                />
              </li>
            );
          })}
        </ol>

        {p.nowMinute != null && p.nowMinute >= h0 * 60 && p.nowMinute <= h1 * 60 ? (
          <div className="pointer-events-none absolute left-14 right-0 z-[5]" style={{ top: top(p.nowMinute) }} aria-hidden>
            <div className="relative h-px bg-alert-text shadow-[0_0_8px_var(--color-alert)]">
              <span className="absolute -left-1 -top-[3px] size-[7px] rounded-full bg-alert-text" />
            </div>
          </div>
        ) : null}
      </div>
    </div>
  );
}

function DetectionBlock({ d, tz, tall, selected, selecting, picked, onClick }: {
  d: Detection;
  tz: string;
  tall: boolean;
  selected: boolean;
  selecting: boolean;
  picked: boolean;
  onClick: () => void;
}) {
  const a = areaMeta(d.area);
  const src = SOURCE_META[detectionSource(d)];
  const status = d.status ?? "detected";
  const canPick = selecting && status === "detected";
  return (
    <button
      type="button"
      onClick={onClick}
      style={areaVars(d.area)}
      aria-pressed={selecting ? picked : selected}
      className={cn(
        "group relative flex h-full w-full flex-col overflow-hidden rounded-lg border-l-[3px] border-[var(--a)] px-2 text-left transition-[box-shadow,background-color,opacity]",
        tall ? "py-1" : "justify-center",
        "bg-[color-mix(in_oklab,var(--a)_13%,var(--color-surface-1))] hover:bg-[color-mix(in_oklab,var(--a)_20%,var(--color-surface-1))]",
        status === "superseded" && "border border-l-[3px] border-dashed !bg-transparent [&_.sum]:text-text-2 [&_.sum]:line-through",
        status === "disputed" && "bg-[repeating-linear-gradient(135deg,color-mix(in_oklab,var(--color-warn)_10%,transparent)_0_6px,transparent_6px_12px)]",
        selected && !selecting && "shadow-[0_0_0_1.5px_var(--color-accent),0_0_18px_-4px_var(--color-accent)]",
        picked && "shadow-[0_0_0_1.5px_var(--color-accent)]",
        selecting && !canPick && "opacity-40",
      )}
    >
      <span className="flex min-w-0 items-center gap-1.5">
        {selecting && canPick ? (
          <span className={cn("grid size-3.5 shrink-0 place-items-center rounded border", picked ? "border-accent bg-accent text-accent-ink" : "border-line-strong")} aria-hidden>
            {picked ? <Check className="size-2.5" strokeWidth={3} /> : null}
          </span>
        ) : null}
        <a.icon className="size-3.5 shrink-0 text-[var(--a)]" aria-hidden />
        <span className="sum min-w-0 flex-1 truncate text-[12.5px] font-medium text-text">{d.summary}</span>
        {status === "confirmed" ? <Check className="size-3.5 shrink-0 text-good" aria-hidden /> : null}
        {!tall ? <src.icon className="size-3 shrink-0 text-text-3" aria-hidden /> : null}
      </span>
      {tall ? (
        <span className="mt-0.5 flex min-w-0 items-center gap-2 text-[11.5px] text-text-3">
          <src.icon className="size-3 shrink-0" aria-hidden />
          <span className="truncate">{d.category}</span>
          <span className="hidden font-mono sm:inline">{fmt(d.started_at, tz, "HH:mm")}–{fmt(d.ended_at, tz, "HH:mm")}</span>
          {status === "detected" ? <ConfidenceMeter value={d.confidence} className="ml-auto shrink-0" /> : null}
          {status === "disputed" ? <span className="ml-auto shrink-0 text-warn">Under review</span> : null}
        </span>
      ) : null}
      <span className="sr-only">, {a.label}, {timeRange(d.started_at, d.ended_at, tz)}, {status === "detected" ? "to review" : status}</span>
    </button>
  );
}
