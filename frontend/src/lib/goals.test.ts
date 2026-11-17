import { describe, expect, it } from "vitest";
import type { Goal } from "@/api/types";
import { allowedParents, buildTree, filterTree, finer } from "./goals";

function g(id: string, horizon: Goal["horizon"], parent_id: string | null = null, extra: Partial<Goal> = {}): Goal {
  return { id, title: id, horizon, parent_id, area: "work", status: "active", version: 1, created_at: "", updated_at: "", created_by: { kind: "user" }, origin: "direct", ...extra };
}

const goals = [g("w1", "week", "m1"), g("m1", "month", "q1"), g("q1", "quarter", "y1"), g("y1", "year"), g("q2", "quarter", null, { status: "achieved" }), g("orphan", "month", "gone")];

describe("goal tree", () => {
  it("nests by parent and puts coarser roots first", () => {
    const t = buildTree(goals);
    expect(t.map((n) => n.goal.id)).toEqual(["y1", "q2", "orphan"]);
    expect(t[0]!.children[0]!.children[0]!.children[0]!.goal.id).toBe("w1");
  });
  it("only offers coarser parents and never a descendant", () => {
    expect(allowedParents(goals, "month").map((x) => x.id).sort()).toEqual(["q1", "q2", "y1"]);
    expect(allowedParents(goals, "quarter", "q1").map((x) => x.id)).toEqual(["y1"]);
    expect(allowedParents(goals, "year")).toEqual([]);
    expect(allowedParents(goals, "week", "y1").map((x) => x.id).sort()).toEqual(["orphan", "q2"]);
  });
  it("filters but keeps ancestors of matches", () => {
    const t = filterTree(buildTree(goals), (x) => x.id === "w1");
    expect(t.map((n) => n.goal.id)).toEqual(["y1"]);
    expect(filterTree(buildTree(goals), (x) => x.status === "achieved").map((n) => n.goal.id)).toEqual(["q2"]);
  });
  it("knows the next finer horizon", () => {
    expect(finer("year")).toBe("quarter");
    expect(finer("week")).toBeNull();
  });
});
