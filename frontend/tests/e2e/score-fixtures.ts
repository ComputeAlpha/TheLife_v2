// Score rows shaped exactly as the scoring engine writes them (backend/src/scoring/engine.cc, jobs.cc), served for
// the /scores calls only: the e2e backend runs with background jobs (and so scoring) off.
import type { Page, Route } from "@playwright/test";
import { dayAt } from "./seed";

const meta = (id: string, version = 1) => ({
  id, version, created_at: "2026-11-10T00:00:00Z", updated_at: "2026-11-10T00:30:00Z", created_by: { kind: "system" }, origin: "direct",
});

function dayScore(offset: number, base: number, pct: number, status = "final") {
  const final = status === "final" ? Math.min(100, Math.max(0, Math.round(base + 0.3 * pct))) : null;
  return {
    ...meta(`day${-offset}`, 2), period: "day", period_start: dayAt(offset), profile: "workday", status, base, adjustment_pct: status === "final" ? pct : null, final,
    formula_version: "2",
    adjustment: status === "final" ? {
      adjustment_pct: pct, goal_alignment: "high",
      reasons: [
        { text: "The afternoon went to the release, the top goal this month", direction: "up", refs: [{ type: "goal", id: "g1" }] },
        { text: "The evening gym block was skipped", direction: "down", refs: [{ type: "task", id: "t1" }] },
      ],
    } : null,
    breakdown: {
      profile: "workday",
      weights: { work: 0.3, skill: 0.25, fitness: 0.15, personal: 0.15, consistency: 0.15 },
      evidence_minutes: { work: 312, skill: 45, entertainment: 50 },
      areas: {
        work: { applicable: true, adherence: 0.86, quantity: 1, alpha: 0.8, bonus: 1.2, score: 89.8,
          items: [{ type: "block", id: "e1", title: "Deep work: backend", weight: 2, completion: 0.93 }, { type: "task", id: "t2", title: "Send weekly report", weight: 1, completion: 1 }],
          targets: [{ id: "tg1", title: "Focused coding", metric: "minutes", amount: 240, achieved: 252, ratio: 1 }] },
        skill: { applicable: true, adherence: 0.5, score: 50, items: [{ type: "block", id: "e2", title: "C++ course", weight: 1, completion: 0.5 }], targets: [] },
        fitness: { applicable: true, adherence: 0, score: 0, items: [{ type: "block", id: "e3", title: "Gym", weight: 1, completion: 0 }], targets: [] },
        personal: { applicable: false, items: [], targets: [] },
      },
      preemptions: [{ kind: "overlap", type: "block", id: "e3", title: "Gym", minutes: 40 }],
    },
    consistency: { score: 71, items: [{ type: "routine", id: "r1", title: "Read 20 pages", completed: 5, applicable: 7, rate: 0.71 }] },
    coverage: null,
  };
}

const days = [dayScore(-5, 64, 0), dayScore(-4, 71, 10), dayScore(-3, 58, -10), { ...meta("day3n"), period: "day", period_start: dayAt(-2), status: "not_scored", profile: "holiday", breakdown: { reason: "the day's profile (holiday) is not scored" }, consistency: {}, base: null, final: null }, dayScore(-1, 74, 13)];
const week = {
  ...meta("week1"), period: "week", period_start: "", status: "final", base: 68.4, final: 68.4, formula_version: "2", adjustment: null, adjustment_pct: null,
  coverage: { scored_days: 4, days: 7 }, consistency: {},
  breakdown: {
    days: days.map((d) => ({ day: d.period_start, status: d.status, ...(d.final != null ? { score: d.final } : {}) })),
    daily_mean: 67.1,
    weekly_targets: [{ id: "wt1", title: "Gym sessions", metric: "sessions", amount: 3, achieved: 2, ratio: 0.67 }],
  },
};

export async function serveScores(page: Page) {
  const disputes: unknown[] = [];
  await page.route(/\/api\/v1\/scores(\/[^?]*)?(\?.*)?$/, async (route: Route) => {
    const req = route.request();
    const url = new URL(req.url());
    if (url.pathname.endsWith(":dispute")) {
      disputes.push(req.postDataJSON());
      return route.fulfill({ status: 202, contentType: "application/json", body: JSON.stringify({ status: "queued" }) });
    }
    if (url.pathname.endsWith("/history")) {
      const cur = days[4]!;
      return route.fulfill({ json: { items: [cur, { ...cur, version: 1, adjustment_pct: 0, final: cur.base, updated_at: "2026-11-10T00:10:00Z" }] } });
    }
    const period = url.searchParams.get("period");
    const from = url.searchParams.get("from") ?? "";
    const items = period === "day" ? days : period === "week" ? [{ ...week, period_start: weekStart(dayAt(-1)) }].filter((w) => w.period_start >= from) : [];
    return route.fulfill({ json: { items, next_cursor: null } });
  });
  return disputes;
}

function weekStart(day: string) {
  const d = new Date(`${day}T00:00:00Z`);
  d.setUTCDate(d.getUTCDate() - ((d.getUTCDay() + 6) % 7));
  return d.toISOString().slice(0, 10);
}

