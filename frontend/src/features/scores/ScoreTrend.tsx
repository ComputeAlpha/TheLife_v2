import { useMemo, useState } from "react";
import type { Score } from "@/api/types";
import { shownScore, type Period } from "@/api/scores";
import { periodLabel } from "@/lib/scores";
import type { Day } from "@/lib/calendar/days";
import { useElementWidth } from "@/lib/useElementWidth";

export const STATUS_LABEL: Record<string, string> = {
  final: "Final",
  provisional: "Provisional: adjustment pending",
  unobserved: "No activity data",
  not_scored: "Not scored (day profile)",
};

interface Point { start: Day; score: Score | null; value: number | null; base: number | null }

const H = 230;
const M = { l: 34, r: 12, t: 14, b: 26 };

/** Final (cyan) and base (dim, dashed) over the window; one y-axis 0–100. */
export function ScoreTrend({ period, starts, byStart, selected, onSelect }: {
  period: Period;
  starts: Day[];
  byStart: Map<Day, Score>;
  selected: Day | null;
  onSelect: (d: Day) => void;
}) {
  const [ref, width] = useElementWidth<HTMLDivElement>();
  const [hover, setHover] = useState<number | null>(null);
  const pts: Point[] = useMemo(
    () =>
      starts.map((start) => {
        const s = byStart.get(start) ?? null;
        return { start, score: s, value: s ? shownScore(s) : null, base: s?.base ?? null };
      }),
    [starts, byStart],
  );
  const n = pts.length;
  const iw = Math.max(0, width - M.l - M.r);
  const ih = H - M.t - M.b;
  const band = n ? iw / n : 0;
  const x = (i: number) => M.l + (i + 0.5) * band;
  const y = (v: number) => M.t + (1 - v / 100) * ih;

  const path = (get: (p: Point) => number | null) => {
    let d = "";
    let pen = false;
    pts.forEach((p, i) => {
      const v = get(p);
      if (v == null) { pen = false; return; }
      d += `${pen ? "L" : "M"}${x(i).toFixed(1)},${y(v).toFixed(1)}`;
      pen = true;
    });
    return d;
  };
  const finalPath = path((p) => p.value);
  const basePath = path((p) => p.base);
  // Area fill under each continuous run of the final line.
  const fill = useMemo(() => {
    const runs: [number, number][][] = [];
    let cur: [number, number][] = [];
    pts.forEach((p, i) => {
      if (p.value == null) { if (cur.length) runs.push(cur); cur = []; return; }
      cur.push([x(i), y(p.value)]);
    });
    if (cur.length) runs.push(cur);
    return runs
      .filter((r) => r.length > 1)
      .map((r) => `M${r[0]![0]},${M.t + ih}` + r.map(([a, b]) => `L${a},${b}`).join("") + `L${r[r.length - 1]![0]},${M.t + ih}Z`)
      .join("");
    // x/y derive from width and n, both captured by pts/width.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pts, width]);

  const labelEvery = Math.max(1, Math.ceil(n / Math.max(2, Math.floor(iw / 72))));
  const selIdx = selected ? starts.indexOf(selected) : -1;
  const hp = hover != null ? pts[hover] : null;

  return (
    <div
      ref={ref}
      className="relative select-none outline-none"
      tabIndex={0}
      role="group"
      aria-label="Score trend. Use the arrow keys to pick a period."
      onKeyDown={(e) => {
        if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
        e.preventDefault();
        const i = Math.max(0, Math.min(n - 1, (selIdx < 0 ? n : selIdx) + (e.key === "ArrowLeft" ? -1 : 1)));
        onSelect(starts[i]!);
      }}
    >
      {width > 0 ? (
        <svg width={width} height={H} className="block" aria-hidden>
          <defs>
            <linearGradient id="score-fill" x1="0" x2="0" y1="0" y2="1">
              <stop offset="0" stopColor="var(--color-accent)" stopOpacity="0.22" />
              <stop offset="1" stopColor="var(--color-accent)" stopOpacity="0" />
            </linearGradient>
          </defs>
          {[0, 25, 50, 75, 100].map((v) => (
            <g key={v}>
              <line x1={M.l} x2={width - M.r} y1={y(v)} y2={y(v)} stroke="var(--color-line)" strokeDasharray={v % 50 ? "2 4" : undefined} />
              {v % 50 === 0 ? <text x={M.l - 8} y={y(v) + 3.5} textAnchor="end" className="fill-text-3 font-mono text-[10.5px]">{v}</text> : null}
            </g>
          ))}
          {pts.map((p, i) => (i % labelEvery === (n - 1) % labelEvery ? (
            <text key={p.start} x={x(i)} y={H - 6} textAnchor="middle" className="fill-text-3 text-[10.5px]">{periodLabel(period, p.start)}</text>
          ) : null))}
          {selIdx >= 0 ? <rect x={x(selIdx) - band / 2} y={M.t} width={band} height={ih} fill="var(--color-accent)" opacity={0.07} rx={4} /> : null}
          {hover != null ? <line x1={x(hover)} x2={x(hover)} y1={M.t} y2={M.t + ih} stroke="var(--color-line-strong)" /> : null}
          <path d={fill} fill="url(#score-fill)" />
          <path d={basePath} fill="none" stroke="var(--color-text-3)" strokeWidth={1.5} strokeDasharray="4 4" strokeLinejoin="round" />
          <path d={finalPath} fill="none" stroke="var(--color-accent)" strokeWidth={2} strokeLinejoin="round" strokeLinecap="round" />
          {pts.map((p, i) => {
            const st = p.score?.status;
            if (!p.score) return null;
            if (p.value == null) {
              // No number: a marker on the baseline, so it never reads as a zero.
              return st === "unobserved"
                ? <circle key={p.start} cx={x(i)} cy={M.t + ih} r={3.5} fill="var(--color-surface-1)" stroke="var(--color-text-3)" strokeWidth={1.5} />
                : <circle key={p.start} cx={x(i)} cy={M.t + ih} r={3.5} fill="var(--color-text-3)" />;
            }
            const hollow = st === "provisional";
            return (
              <g key={p.start}>
                {i === selIdx ? <circle cx={x(i)} cy={y(p.value)} r={8} fill="none" stroke="var(--color-accent)" strokeOpacity={0.45} strokeWidth={2} /> : null}
                <circle cx={x(i)} cy={y(p.value)} r={4.5} fill={hollow ? "var(--color-surface-1)" : "var(--color-accent)"} stroke={hollow ? "var(--color-accent)" : "var(--color-surface-1)"} strokeWidth={2} />
              </g>
            );
          })}
          {pts.map((p, i) => (
            <rect
              key={`hit-${p.start}`}
              x={x(i) - band / 2}
              y={0}
              width={band}
              height={H}
              fill="transparent"
              className={p.score ? "cursor-pointer" : undefined}
              onMouseEnter={() => setHover(i)}
              onMouseLeave={() => setHover((h) => (h === i ? null : h))}
              onClick={() => p.score && onSelect(p.start)}
            />
          ))}
        </svg>
      ) : (
        <div style={{ height: H }} />
      )}
      {hp && hover != null ? (
        <div
          className="pointer-events-none absolute z-10 w-48 rounded-xl border border-line-strong bg-surface-2/95 px-3 py-2 text-[12.5px] shadow-[0_12px_32px_-8px_rgb(0_0_0/.9)] backdrop-blur"
          style={{ left: Math.min(Math.max(0, x(hover) - 96), Math.max(0, width - 192)), top: 0 }}
        >
          <p className="font-medium text-text">{periodLabel(period, hp.start, true)}</p>
          {hp.score ? (
            <>
              {hp.value != null ? (
                <p className="mt-1 flex justify-between text-text-2"><span>Score</span><span className="font-mono text-text">{Math.round(hp.value)}</span></p>
              ) : null}
              {hp.base != null ? (
                <p className="flex justify-between text-text-2"><span>Base</span><span className="font-mono text-text">{Math.round(hp.base)}</span></p>
              ) : null}
              <p className="mt-1 text-text-3">{STATUS_LABEL[hp.score.status ?? "final"]}</p>
            </>
          ) : (
            <p className="mt-1 text-text-3">No score</p>
          )}
        </div>
      ) : null}
    </div>
  );
}

export function TrendTable({ period, starts, byStart, onSelect }: { period: Period; starts: Day[]; byStart: Map<Day, Score>; onSelect: (d: Day) => void }) {
  return (
    <div className="max-h-[260px] overflow-y-auto">
      <table className="w-full text-[13px]">
        <thead className="sticky top-0 bg-surface-1 text-left text-[11px] uppercase tracking-wider text-text-3">
          <tr><th className="py-2 font-semibold">Period</th><th className="font-semibold">Score</th><th className="font-semibold">Base</th><th className="font-semibold">Status</th></tr>
        </thead>
        <tbody className="divide-y divide-line">
          {[...starts].reverse().map((s) => {
            const sc = byStart.get(s);
            const v = sc ? shownScore(sc) : null;
            return (
              <tr key={s}>
                <td className="py-1.5">
                  {sc ? <button className="text-text hover:text-accent" onClick={() => onSelect(s)}>{periodLabel(period, s, true)}</button> : <span className="text-text-3">{periodLabel(period, s, true)}</span>}
                </td>
                <td className="font-mono">{v != null ? Math.round(v) : "–"}</td>
                <td className="font-mono text-text-2">{sc?.base != null ? Math.round(sc.base) : "–"}</td>
                <td className="text-text-3">{sc ? STATUS_LABEL[sc.status ?? "final"] : "No score"}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
