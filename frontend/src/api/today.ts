import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, call } from "./client";
import type { Day } from "@/lib/calendar/days";

export function useProgressToday() {
  return useQuery({
    queryKey: ["progress", "today"],
    queryFn: () => call(api.GET("/api/v1/progress/today")),
    refetchInterval: 5 * 60_000,
  });
}

export function useScoresRange(period: "day" | "week" | "month", from: Day, to: Day) {
  return useQuery({
    queryKey: ["scores", period, from, to],
    queryFn: () => call(api.GET("/api/v1/scores", { params: { query: { period, from, to, limit: 200 } } })),
  });
}

export function usePendingApprovals() {
  return useQuery({
    queryKey: ["approvals", "pending"],
    queryFn: () => call(api.GET("/api/v1/approvals", { params: { query: { status: "pending", limit: 50 } } })),
    refetchInterval: 60_000,
  });
}

export function useDecideApproval() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ id, verb, reason }: { id: string; verb: "approve" | "reject"; reason?: string }) =>
      verb === "approve"
        ? call(api.POST("/api/v1/approvals/{id}:approve", { params: { path: { id } } }))
        : call(api.POST("/api/v1/approvals/{id}:reject", { params: { path: { id } }, body: reason ? { reason } : {} })),
    onSettled: () => {
      qc.invalidateQueries({ queryKey: ["approvals"] });
      // An approved change can touch anything planned.
      for (const k of ["calendar", "tasks", "events", "progress"]) qc.invalidateQueries({ queryKey: [k] });
    },
  });
}

export function useRecentDetections(fromIso: string) {
  return useQuery({
    queryKey: ["detections", "recent", fromIso],
    queryFn: () => call(api.GET("/api/v1/detections", { params: { query: { from: fromIso, limit: 100 } } })),
    refetchInterval: 2 * 60_000,
  });
}

export function useDetectionAction() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ id, verb, reason }: { id: string; verb: "confirm" | "dispute"; reason?: string }) =>
      verb === "confirm"
        ? call(api.POST("/api/v1/detections/{id}:confirm", { params: { path: { id } } }))
        : call(api.POST("/api/v1/detections/{id}:dispute", { params: { path: { id } }, body: { reason: reason || "Not what I was doing" } })),
    onSettled: () => {
      qc.invalidateQueries({ queryKey: ["detections"] });
      qc.invalidateQueries({ queryKey: ["progress"] });
    },
  });
}

export function usePreemptions(from: Day, to: Day) {
  return useQuery({
    queryKey: ["preemptions", from, to],
    queryFn: () => call(api.GET("/api/v1/insights/preemptions", { params: { query: { from, to } } })),
  });
}

export function usePendingReminders() {
  return useQuery({
    queryKey: ["reminders", "pending"],
    queryFn: () => call(api.GET("/api/v1/reminders", { params: { query: { status: "pending", limit: 50 } } })),
  });
}

export function useRoutines() {
  return useQuery({ queryKey: ["routines"], queryFn: () => call(api.GET("/api/v1/routines", { params: { query: { limit: 50 } } })) });
}

export function useDevices() {
  return useQuery({
    queryKey: ["devices"],
    queryFn: () => call(api.GET("/api/v1/devices", { params: { query: { limit: 50 } } })),
    refetchInterval: 60_000,
  });
}
