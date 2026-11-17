import { useMemo, useState } from "react";
import { Navigate, useNavigate, useParams } from "react-router";
import { BarChart3, ChevronLeft, ChevronRight, LineChart, Table2 } from "lucide-react";
import { useUser } from "@/api/auth";
import { useScores, type Period } from "@/api/scores";
import type { Score } from "@/api/types";
import { isValidDay, type Day } from "@/lib/calendar/days";
import { periodEnd, periodStart, periodStarts, stepPeriod, WINDOW } from "@/lib/scores";
import { localDay } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Button, IconButton } from "@/ui/Button";
import { Card } from "@/ui/Card";
import { Segmented } from "@/ui/Segmented";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { Page } from "../common/Page";
import { ScoreDetail } from "./ScoreDetail";
import { ScoreTrend, TrendTable } from "./ScoreTrend";

const PERIODS: Period[] = ["day", "week", "month"];

/** /scores/:period/:date — the date picks the period shown in detail (and the end of the trend window). */
export function ScoresRoute() {
  const { period, date } = useParams();
  const user = useUser();
  const now = useNow();
  const today = localDay(now, user.timezone, user.day_start_hour);
  if (!period || !PERIODS.includes(period as Period)) return <Navigate to="/scores/day" replace />;
  if (date && !isValidDay(date)) return <Navigate to={`/scores/${period}`} replace />;
  return <ScoresPage period={period as Period} date={date ?? null} today={today} />;
}

function ScoresPage({ period, date, today }: { period: Period; date: Day | null; today: Day }) {
  const user = useUser();
  const navigate = useNavigate();
  const [view, setView] = useState<"chart" | "table">("chart");
  // The window ends at the selected period (or now); arrows page it a whole window at a time.
  const [windowEnd, setWindowEnd] = useState<Day>(() => (date && date < today ? date : today));
  const starts = useMemo(() => periodStarts(period, windowEnd), [period, windowEnd]);
  const from = starts[0]!;
  const to = periodEnd(period, starts[starts.length - 1]!);
  const q = useScores(period, from, to);

  const byStart = useMemo(() => {
    const m = new Map<Day, Score>();
    for (const s of q.data?.items ?? []) if (s.period_start) m.set(s.period_start, s);
    return m;
  }, [q.data]);
  // Default selection: the newest period with a number, else the newest row.
  const latest = useMemo(() => {
    const rows = [...byStart.values()].sort((a, b) => b.period_start!.localeCompare(a.period_start!));
    return (rows.find((r) => r.final != null || r.base != null) ?? rows[0])?.period_start ?? null;
  }, [byStart]);
  const selected = date ? periodStart(period, date) : latest;
  const sel = selected ? byStart.get(selected) ?? null : null;

  const select = (d: Day) => navigate(`/scores/${period}/${d}`, { replace: true });
  const atEnd = periodStart(period, windowEnd) >= periodStart(period, today);

  return (
    <Page
      wide
      title="Scores"
      subtitle="How the plan went, from what was done and seen. Every adjustment comes with its reasons."
      actions={
        <Segmented<Period>
          label="Period"
          value={period}
          onChange={(p) => { setWindowEnd(today); navigate(`/scores/${p}`); }}
          options={PERIODS.map((p) => ({ value: p, label: p[0]!.toUpperCase() + p.slice(1) }))}
        />
      }
    >
      <Card className="mb-5 p-5">
        <div className="mb-3 flex flex-wrap items-center gap-x-5 gap-y-2">
          <h2 className="text-sm font-semibold">Last {WINDOW[period]} {period}s</h2>
          <ul className="flex flex-wrap items-center gap-x-4 gap-y-1 text-[12px] text-text-2" aria-label="Legend">
            <li className="flex items-center gap-1.5"><span className="h-0.5 w-4 rounded bg-accent" /> Score</li>
            <li className="flex items-center gap-1.5"><span className="h-0 w-4 border-t-[1.5px] border-dashed border-text-3" /> Base</li>
            <li className="flex items-center gap-1.5"><span className="size-2.5 rounded-full border-2 border-accent" /> Provisional</li>
            <li className="flex items-center gap-1.5"><span className="size-2 rounded-full bg-text-3" /> Not scored</li>
          </ul>
          <div className="ml-auto flex items-center gap-1">
            <IconButton size="sm" label="Earlier" onClick={() => setWindowEnd(stepPeriod(period, periodStart(period, windowEnd), -WINDOW[period]))}><ChevronLeft className="size-4" /></IconButton>
            <IconButton size="sm" label="Later" disabled={atEnd} onClick={() => {
              const next = stepPeriod(period, periodStart(period, windowEnd), WINDOW[period]);
              setWindowEnd(next > today ? today : next);
            }}><ChevronRight className="size-4" /></IconButton>
            <IconButton size="sm" label={view === "chart" ? "Show as table" : "Show as chart"} onClick={() => setView(view === "chart" ? "table" : "chart")}>
              {view === "chart" ? <Table2 className="size-4" /> : <LineChart className="size-4" />}
            </IconButton>
          </div>
        </div>
        {q.isPending ? (
          <Skeleton className="h-[230px]" />
        ) : q.error ? (
          <ErrorState error={q.error} retry={() => q.refetch()} />
        ) : view === "chart" ? (
          <ScoreTrend period={period} starts={starts} byStart={byStart} selected={selected} onSelect={select} />
        ) : (
          <TrendTable period={period} starts={starts} byStart={byStart} onSelect={select} />
        )}
      </Card>

      {q.isPending ? (
        <Skeleton className="h-80" />
      ) : sel ? (
        <ScoreDetail key={sel.id} s={sel} period={period} tz={user.timezone} onOpenDay={(d) => navigate(`/scores/day/${d}`)} />
      ) : !q.error ? (
        <Card>
          <EmptyState
            art={<BarChart3 className="size-10 text-accent" />}
            title={byStart.size ? "No score for this period" : "No scores yet"}
            body={byStart.size
              ? "Pick a point on the chart."
              : period === "day"
                ? "A day is scored shortly after it ends, from what was planned, what got done and what the collector saw."
                : `A ${period} is scored when it closes, from its day scores.`}
            action={date ? <Button onClick={() => navigate(`/scores/${period}`)}>Show the latest</Button> : undefined}
          />
        </Card>
      ) : null}
    </Page>
  );
}
