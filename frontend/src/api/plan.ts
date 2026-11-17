// Goals, plans and targets (frontend_lld.md §6.8).
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import type { Goal, GoalCreate, GoalPatch, Plan, PlanCreate, PlanPatch, Target, TargetCreate, TargetPatch } from "./types";

export function useGoals() {
  return useQuery({
    queryKey: ["goals", "all"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/goals", { params: { query: { limit: 200, cursor } } }))),
  });
}
export function usePlans() {
  return useQuery({
    queryKey: ["plans", "all"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/plans", { params: { query: { limit: 200, cursor } } }))),
  });
}
export function useTargets() {
  return useQuery({
    queryKey: ["targets", "all"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/targets", { params: { query: { limit: 200, cursor } } }))),
  });
}
/** Open tasks, for the per-goal counts. */
export function useOpenTasks() {
  return useQuery({
    queryKey: ["tasks", "open"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/tasks", { params: { query: { status: "open", limit: 200, cursor } } }))),
  });
}

function useInvalidate() {
  const qc = useQueryClient();
  return () => {
    for (const k of ["goals", "plans", "targets", "progress", "tasks"]) qc.invalidateQueries({ queryKey: [k] });
  };
}

export function useSaveGoal() {
  const done = useInvalidate();
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: Goal; body: GoalCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/goals/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body: body as GoalPatch, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/goals", { body })),
    onSettled: done,
  });
}
export function useDeleteGoal() {
  const done = useInvalidate();
  return useMutation({
    mutationFn: (g: Goal) => call(api.DELETE("/api/v1/goals/{id}", { params: { path: { id: g.id }, header: ifMatch(g.version) } })),
    onSettled: done,
  });
}

export function useSavePlan() {
  const done = useInvalidate();
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: Plan; body: PlanCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/plans/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body: body as PlanPatch, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/plans", { body })),
    onSettled: done,
  });
}
export function useDeletePlan() {
  const done = useInvalidate();
  return useMutation({
    mutationFn: (p: Plan) => call(api.DELETE("/api/v1/plans/{id}", { params: { path: { id: p.id }, header: ifMatch(p.version) } })),
    onSettled: done,
  });
}

export function useSaveTarget() {
  const done = useInvalidate();
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: Target; body: TargetCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/targets/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body: body as TargetPatch, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/targets", { body })),
    onSettled: done,
  });
}
export function useDeleteTarget() {
  const done = useInvalidate();
  return useMutation({
    mutationFn: (t: Target) => call(api.DELETE("/api/v1/targets/{id}", { params: { path: { id: t.id }, header: ifMatch(t.version) } })),
    onSettled: done,
  });
}
