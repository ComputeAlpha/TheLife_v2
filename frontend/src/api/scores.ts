// Scores (frontend_lld.md §6.7). The JSON columns are open in the contract; their shapes are the scoring engine's
// (backend/src/scoring/engine.cc scoreDay, jobs.cc closePeriod and adjust) and are typed here.
import { keepPreviousData, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, call } from "./client";
import type { Score } from "./types";
import type { Day } from "@/lib/calendar/days";

export type Period = "day" | "week" | "month";

export interface BreakdownItem { type: string; id: string; title: string; weight: number; completion: number; excluded?: boolean }
export interface BreakdownTarget { id: string; title: string; metric: string; amount: number; achieved: number; ratio: number }
export interface AreaBreakdown {
  applicable: boolean;
  items: BreakdownItem[];
  targets: BreakdownTarget[];
  adherence?: number;
  quantity?: number;
  alpha?: number;
  bonus?: number;
  score?: number;
}
export interface Preemption { kind: "overlap" | "while_due"; type: string; id: string; title: string; minutes: number }
export interface DayBreakdown {
  areas?: Record<string, AreaBreakdown>;
  weights?: Record<string, number>;
  profile?: string;
  evidence_minutes?: Record<string, number>;
  preemptions?: Preemption[];
  reason?: string;
  // period scores
  days?: { day: Day; status: string; score?: number }[];
  weekly_targets?: BreakdownTarget[];
  daily_mean?: number;
}
export interface ConsistencyItem { type: "routine" | "target"; id: string; title: string; completed: number; applicable: number; rate: number }
export interface Consistency { items?: ConsistencyItem[]; score?: number }
export interface AdjustmentReason { text: string; direction: "up" | "down"; refs?: { type: string; id: string }[] }
export interface Adjustment { adjustment_pct?: number; reasons?: AdjustmentReason[]; goal_alignment?: "low" | "medium" | "high" }
export interface Coverage { scored_days: number; days: number }

/** The typed view of a score row's JSON columns. */
export function readScore(s: Score) {
  return {
    breakdown: (s.breakdown ?? {}) as DayBreakdown,
    consistency: (s.consistency ?? {}) as Consistency,
    adjustment: (s.adjustment ?? null) as Adjustment | null,
    coverage: (s.coverage ?? null) as Coverage | null,
  };
}

/** The number to show: final when there is one, else base (provisional rows carry base only). */
export function shownScore(s: Score): number | null {
  return s.final ?? s.base ?? null;
}

export function useScores(period: Period, from: Day, to: Day) {
  return useQuery({
    queryKey: ["scores", period, from, to],
    placeholderData: keepPreviousData,
    queryFn: () => call(api.GET("/api/v1/scores", { params: { query: { period, from, to, limit: 200 } } })),
  });
}

export function useScoreHistory(id: string | null) {
  return useQuery({
    queryKey: ["scores", "history", id],
    enabled: !!id,
    queryFn: () => call(api.GET("/api/v1/scores/{id}/history", { params: { path: { id: id! } } })),
  });
}

export function useDisputeScore() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ id, reason }: { id: string; reason: string }) =>
      call(api.POST("/api/v1/scores/{id}:dispute", { params: { path: { id } }, body: { reason } })),
    onSettled: () => qc.invalidateQueries({ queryKey: ["scores"] }),
  });
}
