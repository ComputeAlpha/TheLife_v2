import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, call, newId } from "./client";
import type { components } from "./schema";
import type { Day } from "@/lib/calendar/days";

export type ChatMessage = components["schemas"]["ChatMessage"];
export type Conversation = components["schemas"]["Conversation"];
export type ChatTurn = components["schemas"]["ChatTurn"];

export function useConversations() {
  return useQuery({
    queryKey: ["conversations", "all"],
    queryFn: () => call(api.GET("/api/v1/conversations", { params: { query: { limit: 100 } } })),
  });
}

/** Today's web conversation (one per channel per local day); null until the first message of the day. */
export function useTodayConversation(today: Day) {
  return useQuery({
    queryKey: ["conversations", "web", today],
    queryFn: async () => {
      const r = await call(api.GET("/api/v1/conversations", { params: { query: { channel: "web", from: today, to: today, limit: 5 } } }));
      return r.items[0] ?? null;
    },
  });
}

/** All messages of a conversation (pages of 500 until done). */
export function useMessages(conversationId: string | null | undefined) {
  return useQuery({
    queryKey: ["messages", conversationId],
    enabled: !!conversationId,
    queryFn: async () => {
      const out: ChatMessage[] = [];
      let after: string | undefined;
      for (let i = 0; i < 20; i++) {
        const r = await call(api.GET("/api/v1/conversations/{id}/messages", { params: { path: { id: conversationId! }, query: { limit: 500, ...(after ? { after } : {}) } } }));
        out.push(...r.items);
        if (r.items.length < 500) break;
        after = r.items.at(-1)!.id;
      }
      return out;
    },
    placeholderData: (prev) => prev,
  });
}

export interface Outgoing {
  clientId: string;
  text: string;
  turn?: ChatTurn;
  error?: unknown;
}

export function useSendMessage() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ text, clientId }: { text: string; clientId: string }) =>
      call(api.POST("/api/v1/chat/web/messages", { body: { text, client_msg_id: clientId } })),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ["conversations"] });
      qc.invalidateQueries({ queryKey: ["messages"] });
    },
  });
}

export function useTurn(id: string | null) {
  return useQuery({
    queryKey: ["turn", id],
    enabled: !!id,
    queryFn: () => call(api.GET("/api/v1/chat/turns/{id}", { params: { path: { id: id! } } })),
  });
}

export function useApproval(id: string) {
  return useQuery({
    queryKey: ["approvals", "one", id],
    queryFn: () => call(api.GET("/api/v1/approvals/{id}", { params: { path: { id } } })),
  });
}

export function useApprovals(status: "pending" | "approved" | "rejected" | "expired" | "stale" | "failed" | undefined) {
  return useQuery({
    queryKey: ["approvals", "list", status ?? "all"],
    queryFn: () => call(api.GET("/api/v1/approvals", { params: { query: { limit: 100, ...(status ? { status } : {}) } } })),
  });
}

export { newId };
