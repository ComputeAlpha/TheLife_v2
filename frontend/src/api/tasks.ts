// Tasks (frontend_lld.md §6.9).
import { useMutation, useQuery } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import { useInvalidate } from "./invalidate";
import type { Task, TaskCreate, TaskPatch } from "./types";
import type { Day } from "@/lib/calendar/days";

/** Every task in a status (open by default), all pages. */
export function useTaskList(status: "open" | "done" | "cancelled") {
  return useQuery({
    queryKey: ["tasks", "list", status],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/tasks", { params: { query: { status, limit: 200, cursor } } })), status === "open" ? 10 : 2),
  });
}

export function useTaskOccurrences(id: string | null, from: Day, to: Day) {
  return useQuery({
    queryKey: ["tasks", "occurrences", id, from, to],
    enabled: !!id,
    queryFn: () => call(api.GET("/api/v1/tasks/{id}/occurrences", { params: { path: { id: id! }, query: { from, to } } })),
  });
}

const FAMILIES = ["tasks", "calendar", "progress", "goals"];

export function useSaveTask() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: Task; body: TaskCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/tasks/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body: body as TaskPatch, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/tasks", { body })),
    onSettled: done,
  });
}

/** Status changes from a list: fetches the current version first (rows can be stale). */
export function useSetTaskStatus() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: async ({ id, status }: { id: string; status: "open" | "done" | "cancelled" }) => {
      const t = await call(api.GET("/api/v1/tasks/{id}", { params: { path: { id } } }));
      return call(api.PATCH("/api/v1/tasks/{id}", { params: { path: { id }, header: ifMatch(t.version) }, body: { status }, headers: MERGE_PATCH }));
    },
    onSettled: done,
  });
}

export function useDeleteTask() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: (t: Task) => call(api.DELETE("/api/v1/tasks/{id}", { params: { path: { id: t.id }, header: ifMatch(t.version) } })),
    onSettled: done,
  });
}

export function useOccurrenceAction() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ id, date, verb, reason }: { id: string; date: Day; verb: "complete" | "skip"; reason?: string }) =>
      verb === "complete"
        ? call(api.POST("/api/v1/tasks/{id}/occurrences/{date}:complete", { params: { path: { id, date } }, body: {} }))
        : call(api.POST("/api/v1/tasks/{id}/occurrences/{date}:skip", { params: { path: { id, date } }, body: reason ? { reason } : {} })),
    onSettled: done,
  });
}
