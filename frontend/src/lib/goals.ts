// Goal tree helpers (frontend_lld.md §6.8): year → quarter → month → week.
import type { Goal } from "@/api/types";

export const HORIZONS = ["year", "quarter", "month", "week"] as const;
export type Horizon = (typeof HORIZONS)[number];
const RANK: Record<Horizon, number> = { year: 0, quarter: 1, month: 2, week: 3 };

export interface GoalNode { goal: Goal; children: GoalNode[] }

/** Roots are goals without a (visible) parent. Siblings: coarser horizon first, then due date, then title. */
export function buildTree(goals: Goal[]): GoalNode[] {
  const nodes = new Map(goals.map((g) => [g.id, { goal: g, children: [] as GoalNode[] }]));
  const roots: GoalNode[] = [];
  for (const n of nodes.values()) {
    const parent = n.goal.parent_id ? nodes.get(n.goal.parent_id) : undefined;
    (parent ? parent.children : roots).push(n);
  }
  const sort = (list: GoalNode[]) => {
    list.sort((a, b) =>
      RANK[a.goal.horizon] - RANK[b.goal.horizon] ||
      (a.goal.due_on ?? "9999").localeCompare(b.goal.due_on ?? "9999") ||
      a.goal.title.localeCompare(b.goal.title));
    for (const n of list) sort(n.children);
  };
  sort(roots);
  return roots;
}

/** Parents a goal of `horizon` may have: strictly coarser horizons, never itself or its own descendants. */
export function allowedParents(goals: Goal[], horizon: Horizon, selfId?: string): Goal[] {
  const blocked = new Set<string>();
  if (selfId) {
    blocked.add(selfId);
    let grew = true;
    while (grew) {
      grew = false;
      for (const g of goals) {
        if (g.parent_id && blocked.has(g.parent_id) && !blocked.has(g.id)) {
          blocked.add(g.id);
          grew = true;
        }
      }
    }
  }
  return goals.filter((g) => RANK[g.horizon] < RANK[horizon] && !blocked.has(g.id));
}

/** The next finer horizon (for "add a sub-goal"); weeks have none. */
export function finer(h: Horizon): Horizon | null {
  return HORIZONS[RANK[h] + 1] ?? null;
}

/** Keeps nodes that match, plus the ancestors needed to reach them. */
export function filterTree(nodes: GoalNode[], keep: (g: Goal) => boolean): GoalNode[] {
  const out: GoalNode[] = [];
  for (const n of nodes) {
    const children = filterTree(n.children, keep);
    if (keep(n.goal) || children.length) out.push({ goal: n.goal, children });
  }
  return out;
}
