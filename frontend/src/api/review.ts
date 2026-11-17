// Activity review (frontend_lld.md §6.5): detections and the manual activity log.
import { keepPreviousData, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import type { Activity, ActivityCreate, ActivityPatch, Area } from "./types";

/** Every detection overlapping [from, to), superseded ones included (the page decides what to show). */
export function useDetectionsIn(fromIso: string, toIso: string) {
  return useQuery({
    queryKey: ["detections", "day", fromIso, toIso],
    placeholderData: keepPreviousData,
    queryFn: () =>
      collect((cursor) => call(api.GET("/api/v1/detections", { params: { query: { from: fromIso, to: toIso, limit: 200, cursor } } }))),
  });
}

export function useDetectionHistory(id: string | null) {
  return useQuery({
    queryKey: ["detections", "history", id],
    enabled: !!id,
    queryFn: () => call(api.GET("/api/v1/detections/{id}/history", { params: { path: { id: id! } } })),
  });
}

function useReviewInvalidation() {
  const qc = useQueryClient();
  return () => {
    for (const k of ["detections", "activities", "progress", "scores"]) qc.invalidateQueries({ queryKey: [k] });
  };
}

export function useConfirmDetections() {
  const done = useReviewInvalidation();
  return useMutation({
    mutationFn: async (ids: string[]) => {
      // One at a time keeps the server's per-row audit simple; bulk review is a few dozen at most.
      for (const id of ids) await call(api.POST("/api/v1/detections/{id}:confirm", { params: { path: { id } } }));
      return ids.length;
    },
    onSettled: done,
  });
}

export function useDisputeDetection() {
  const done = useReviewInvalidation();
  return useMutation({
    mutationFn: ({ id, reason }: { id: string; reason: string }) =>
      call(api.POST("/api/v1/detections/{id}:dispute", { params: { path: { id } }, body: { reason: reason.trim() || "Not what I was doing" } })),
    onSettled: done,
  });
}

export interface Correction {
  area: Area;
  category: string;
  summary: string;
  started_at?: string;
  ended_at?: string;
}

/** "Correct": claim the time with what really happened; the server supersedes the original. */
export function useCorrectDetection() {
  const done = useReviewInvalidation();
  return useMutation({
    mutationFn: ({ id, body }: { id: string; body: Correction }) =>
      call(api.POST("/api/v1/detections/{id}:claim", { params: { path: { id } }, body })),
    onSettled: done,
  });
}

// ---- activities (the manual log)

export function useActivitiesIn(fromIso: string, toIso: string) {
  return useQuery({
    queryKey: ["activities", fromIso, toIso],
    placeholderData: keepPreviousData,
    queryFn: () =>
      collect((cursor) => call(api.GET("/api/v1/activities", { params: { query: { from: fromIso, to: toIso, limit: 200, cursor } } }))),
  });
}

export function useSaveActivity() {
  const done = useReviewInvalidation();
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: Activity; body: ActivityCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/activities/{id}", {
            params: { path: { id: existing.id }, header: ifMatch(existing.version) },
            body: body as ActivityPatch,
            headers: MERGE_PATCH,
          }))
        : call(api.POST("/api/v1/activities", { body })),
    onSettled: done,
  });
}

export function useDeleteActivity() {
  const done = useReviewInvalidation();
  return useMutation({
    mutationFn: (a: Activity) => call(api.DELETE("/api/v1/activities/{id}", { params: { path: { id: a.id }, header: ifMatch(a.version) } })),
    onSettled: done,
  });
}

/** "Add what you were doing": a user-entered detection for time the collector didn't see. */
export function useAddDetection() {
  const done = useReviewInvalidation();
  return useMutation({
    mutationFn: (body: Correction & { started_at: string; ended_at: string }) =>
      call(api.POST("/api/v1/detections", { body: { source: "user", confidence: 1, ...body } })),
    onSettled: done,
  });
}
