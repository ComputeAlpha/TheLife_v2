// Assistant: routines, reminders and memory (frontend_lld.md §6.13).
import { useMutation, useQuery } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import { useInvalidate } from "./invalidate";
import type { MemoryFact, Reminder, ReminderCreate, Routine, RoutineCreate } from "./types";
import type { Day } from "@/lib/calendar/days";

export function useRoutineList() {
  return useQuery({ queryKey: ["routines", "all"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/routines", { params: { query: { limit: 200, cursor } } }))) });
}
export function useReminderList() {
  return useQuery({ queryKey: ["reminders", "all"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/reminders", { params: { query: { limit: 200, cursor } } })), 3) });
}
export function useMemoryFacts(status: "active" | "proposed" | "superseded" | undefined) {
  return useQuery({
    queryKey: ["memory", status ?? "all"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/memory/facts", { params: { query: { limit: 200, cursor, ...(status ? { status } : {}) } } })), 5),
  });
}

export function useSaveRoutine() {
  const done = useInvalidate("routines", "progress", "scores");
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: Routine; body: Partial<RoutineCreate> }) =>
      existing
        ? call(api.PATCH("/api/v1/routines/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/routines", { body: body as RoutineCreate })),
    onSettled: done,
  });
}
export function useDeleteRoutine() {
  const done = useInvalidate("routines");
  return useMutation({ mutationFn: (x: Routine) => call(api.DELETE("/api/v1/routines/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}
export function useSkipRoutine() {
  const done = useInvalidate("routines");
  return useMutation({ mutationFn: ({ id, date }: { id: string; date: Day }) => call(api.POST("/api/v1/routines/{id}:skip", { params: { path: { id } }, body: { date } })), onSettled: done });
}

export function useSaveReminder() {
  const done = useInvalidate("reminders");
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: Reminder; body: Partial<ReminderCreate> }) =>
      existing
        ? call(api.PATCH("/api/v1/reminders/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/reminders", { body: body as ReminderCreate })),
    onSettled: done,
  });
}
export function useDeleteReminder() {
  const done = useInvalidate("reminders");
  return useMutation({ mutationFn: (x: Reminder) => call(api.DELETE("/api/v1/reminders/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}

/** "Forget": deletes the fact (the agent never sees it again). */
export function useForgetFact() {
  const done = useInvalidate("memory");
  return useMutation({ mutationFn: (x: MemoryFact) => call(api.DELETE("/api/v1/memory/facts/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}
