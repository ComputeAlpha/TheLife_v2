import { useMutation, useQueries, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import type { Area, CalendarEntry, CalendarEventCreate, CalendarEventPatch } from "./types";
import { windowInstants, windows } from "@/lib/calendar/entries";
import type { Day } from "@/lib/calendar/days";

/** Everything planned between two days (any length; fetched in ≤ 62-day windows, in parallel). */
export function useCalendar(from: Day, to: Day, tz: string) {
  const parts = windows(from, to);
  const qs = useQueries({
    queries: parts.map(([f, t]) => ({
      queryKey: ["calendar", tz, f, t],
      queryFn: () => call(api.GET("/api/v1/calendar", { params: { query: windowInstants(f, t, tz) } })),
      placeholderData: (prev: { items: CalendarEntry[]; truncated: boolean } | undefined) => prev,
    })),
  });
  const seen = new Set<string>();
  const items: CalendarEntry[] = [];
  for (const q of qs) {
    for (const e of q.data?.items ?? []) {
      const k = `${e.kind}:${e.id}:${e.occurs_on ?? e.starts_at}`;
      if (!seen.has(k)) {
        seen.add(k);
        items.push(e);
      }
    }
  }
  return {
    items,
    truncated: qs.some((q) => q.data?.truncated),
    isPending: qs.some((q) => q.isPending),
    isFetching: qs.some((q) => q.isFetching),
    error: qs.find((q) => q.error)?.error ?? null,
    refetch: () => qs.forEach((q) => q.refetch()),
  };
}

/** Invalidates everything a planning change can show up in. */
export function usePlanningInvalidation() {
  const qc = useQueryClient();
  return () => {
    for (const k of ["calendar", "events", "tasks", "progress", "slots", "sessions"]) qc.invalidateQueries({ queryKey: [k] });
  };
}

export function useEvent(id: string | null) {
  return useQuery({
    queryKey: ["events", id],
    enabled: !!id,
    queryFn: () => call(api.GET("/api/v1/events/{id}", { params: { path: { id: id! } } })),
  });
}

export function useCreateEvent() {
  const done = usePlanningInvalidation();
  return useMutation({ mutationFn: (body: CalendarEventCreate) => call(api.POST("/api/v1/events", { body })), onSuccess: done });
}

/** PATCH with the current version (fetched when the caller doesn't have it). */
export function usePatchEvent() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: async ({ id, patch, version }: { id: string; patch: CalendarEventPatch; version?: number }) => {
      const v = version ?? (await call(api.GET("/api/v1/events/{id}", { params: { path: { id } } }))).version;
      return call(api.PATCH("/api/v1/events/{id}", { params: { path: { id }, header: ifMatch(v) }, body: patch, headers: MERGE_PATCH }));
    },
    onSettled: done,
  });
}

export function useDeleteEvent() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: async ({ id, version }: { id: string; version?: number }) => {
      const v = version ?? (await call(api.GET("/api/v1/events/{id}", { params: { path: { id } } }))).version;
      return call(api.DELETE("/api/v1/events/{id}", { params: { path: { id }, header: ifMatch(v) } }));
    },
    onSuccess: done,
  });
}

export function useTask(id: string | null) {
  return useQuery({
    queryKey: ["tasks", id],
    enabled: !!id,
    queryFn: () => call(api.GET("/api/v1/tasks/{id}", { params: { path: { id: id! } } })),
  });
}

/**
 * Completes or skips a task on a day: recurring tasks act on that occurrence; one-off tasks change their status
 * (the occurrence endpoints refuse them).
 */
export function useTaskAction() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: async ({ entry, verb, reason }: { entry: CalendarEntry; verb: "complete" | "skip" | "reopen"; reason?: string }) => {
      const id = entry.id;
      if (entry.all_day && entry.occurs_on && verb !== "reopen") {
        const path = { id, date: entry.occurs_on };
        return verb === "complete"
          ? call(api.POST("/api/v1/tasks/{id}/occurrences/{date}:complete", { params: { path }, body: {} }))
          : call(api.POST("/api/v1/tasks/{id}/occurrences/{date}:skip", { params: { path }, body: reason ? { reason } : {} }));
      }
      const t = await call(api.GET("/api/v1/tasks/{id}", { params: { path: { id } } }));
      const status = verb === "complete" ? "done" : verb === "skip" ? "cancelled" : "open";
      return call(api.PATCH("/api/v1/tasks/{id}", { params: { path: { id }, header: ifMatch(t.version) }, body: { status }, headers: MERGE_PATCH }));
    },
    onSuccess: done,
  });
}

export function useCreateTask() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: (body: { title: string; area: Area; due_at?: string; rrule?: string | null; rrule_dtstart?: string; estimate_min?: number }) =>
      call(api.POST("/api/v1/tasks", { body })),
    onSuccess: done,
  });
}

export function usePatchTaskDue() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: async ({ id, due_at }: { id: string; due_at: string }) => {
      const t = await call(api.GET("/api/v1/tasks/{id}", { params: { path: { id } } }));
      return call(api.PATCH("/api/v1/tasks/{id}", { params: { path: { id }, header: ifMatch(t.version) }, body: { due_at }, headers: MERGE_PATCH }));
    },
    onSettled: done,
  });
}

export function useSkillResources() {
  return useQuery({
    queryKey: ["skill-resources"],
    queryFn: () => call(api.GET("/api/v1/skills/resources", { params: { query: { limit: 200 } } })),
    staleTime: 60_000,
  });
}
export function useEntertainmentItems() {
  return useQuery({
    queryKey: ["entertainment-items"],
    queryFn: () => call(api.GET("/api/v1/entertainment/items", { params: { query: { limit: 200 } } })),
    staleTime: 60_000,
  });
}

export function useCreateSession() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: (body: { resource_id: string; started_at: string; ended_at: string; notes?: string }) =>
      call(api.POST("/api/v1/skills/sessions", { body })),
    onSuccess: done,
  });
}
export function useCreateSlot() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: (body: { item_id?: string | null; starts_at: string; ends_at: string; notes?: string }) =>
      call(api.POST("/api/v1/entertainment/slots", { body })),
    onSuccess: done,
  });
}

/** Moves or edits a slot or a skill session (both carry a version; fetched first). */
export function usePatchTimed() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: async ({ kind, id, patch }: { kind: "entertainment_slot" | "skill_session"; id: string; patch: Record<string, unknown> }) => {
      if (kind === "entertainment_slot") {
        const cur = await call(api.GET("/api/v1/entertainment/slots/{id}", { params: { path: { id } } }));
        return call(api.PATCH("/api/v1/entertainment/slots/{id}", { params: { path: { id }, header: ifMatch(cur.version) }, body: patch, headers: MERGE_PATCH }));
      }
      const cur = await call(api.GET("/api/v1/skills/sessions/{id}", { params: { path: { id } } }));
      const body = { ...patch } as Record<string, unknown>;
      if ("starts_at" in body) {
        body.started_at = body.starts_at;
        delete body.starts_at;
      }
      if ("ends_at" in body) {
        body.ended_at = body.ends_at;
        delete body.ends_at;
      }
      return call(api.PATCH("/api/v1/skills/sessions/{id}", { params: { path: { id }, header: ifMatch(cur.version) }, body, headers: MERGE_PATCH }));
    },
    onSettled: done,
  });
}

export function useDeleteTimed() {
  const done = usePlanningInvalidation();
  return useMutation({
    mutationFn: async ({ kind, id }: { kind: "entertainment_slot" | "skill_session"; id: string }) => {
      if (kind === "entertainment_slot") {
        const cur = await call(api.GET("/api/v1/entertainment/slots/{id}", { params: { path: { id } } }));
        return call(api.DELETE("/api/v1/entertainment/slots/{id}", { params: { path: { id }, header: ifMatch(cur.version) } }));
      }
      const cur = await call(api.GET("/api/v1/skills/sessions/{id}", { params: { path: { id } } }));
      return call(api.DELETE("/api/v1/skills/sessions/{id}", { params: { path: { id }, header: ifMatch(cur.version) } }));
    },
    onSuccess: done,
  });
}

// ---- days: scores, profiles, overrides

export function useDayScores(from: Day, to: Day) {
  return useQuery({
    queryKey: ["scores", "day", from, to],
    queryFn: () => call(api.GET("/api/v1/scores", { params: { query: { period: "day", from, to, limit: 200 } } })),
  });
}

export function useDayProfiles() {
  return useQuery({
    queryKey: ["day-profiles"],
    queryFn: () => call(api.GET("/api/v1/day-profiles", { params: { query: { limit: 200 } } })),
    staleTime: 5 * 60_000,
  });
}

export function useOverrides(from: Day, to: Day) {
  return useQuery({
    queryKey: ["overrides", from, to],
    queryFn: () => call(api.GET("/api/v1/day-profile-overrides", { params: { query: { from, to, limit: 200 } } })),
  });
}

export function useSetOverride() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: async ({ day, profile, existing }: { day: Day; profile: string | null; existing?: { id: string; version: number } }) => {
      if (existing) {
        await call(api.DELETE("/api/v1/day-profile-overrides/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) } }));
      }
      if (profile) return call(api.POST("/api/v1/day-profile-overrides", { body: { local_day: day, profile } }));
      return null;
    },
    onSettled: () => {
      qc.invalidateQueries({ queryKey: ["overrides"] });
      qc.invalidateQueries({ queryKey: ["progress"] });
    },
  });
}

export function useDetectionsBetween(fromIso: string, toIso: string, enabled: boolean) {
  return useQuery({
    queryKey: ["detections", "window", fromIso, toIso],
    enabled,
    queryFn: () => call(api.GET("/api/v1/detections", { params: { query: { from: fromIso, to: toIso, limit: 200 } } })),
  });
}

/** Built-in day profiles (scoring_lld.md §3), merged with the user's own. */
export const BUILTIN_PROFILES = ["workday", "weekend", "holiday", "leave", "sick"];
