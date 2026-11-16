import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, call } from "./client";

export function useOpenNotifications() {
  return useQuery({
    queryKey: ["notifications", "open"],
    queryFn: () => call(api.GET("/api/v1/notifications", { params: { query: { open: true, limit: 50 } } })),
    refetchInterval: 2 * 60_000,
  });
}

export function useRecentNotifications(enabled: boolean) {
  return useQuery({
    queryKey: ["notifications", "recent"],
    enabled,
    queryFn: () => call(api.GET("/api/v1/notifications", { params: { query: { limit: 30 } } })),
  });
}

export function ackNotification(id: string, clicked: boolean) {
  return call(api.POST("/api/v1/notifications/{id}:ack", { params: { path: { id } }, body: { clicked } }));
}

export function useAckNotification() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ id, clicked }: { id: string; clicked: boolean }) => ackNotification(id, clicked),
    onSettled: () => qc.invalidateQueries({ queryKey: ["notifications"] }),
  });
}

/** Where a notification leads: its approval in chat, or the chat. */
export function notificationHref(n: { action?: string | null; ref_type?: string | null; ref_id?: string | null }): string {
  if ((n.action === "open_approval" || n.ref_type === "approval") && n.ref_id) return `/chat#approval-${n.ref_id}`;
  return "/chat";
}
