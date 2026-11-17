// Entertainment: items and planned slots (frontend_lld.md §6.12). Planned, never limited.
import { useMutation, useQuery } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import { useInvalidate } from "./invalidate";
import type { EntertainmentItem, EntertainmentItemCreate, EntertainmentSlot, EntertainmentSlotCreate } from "./types";

export function useItems() {
  return useQuery({
    queryKey: ["entertainment-items", "all"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/entertainment/items", { params: { query: { limit: 200, cursor } } }))),
  });
}
export function useSlots(fromIso: string, toIso: string) {
  return useQuery({
    queryKey: ["slots", fromIso, toIso],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/entertainment/slots", { params: { query: { from: fromIso, to: toIso, limit: 200, cursor } } }))),
  });
}

const FAMILIES = ["entertainment-items", "slots", "calendar", "progress"];

export function useSaveItem() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: EntertainmentItem; body: EntertainmentItemCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/entertainment/items/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/entertainment/items", { body })),
    onSettled: done,
  });
}
export function useDeleteItem() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: (x: EntertainmentItem) => call(api.DELETE("/api/v1/entertainment/items/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })),
    onSettled: done,
  });
}
export function useSaveSlot() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: EntertainmentSlot; body: Partial<EntertainmentSlotCreate> }) =>
      existing
        ? call(api.PATCH("/api/v1/entertainment/slots/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/entertainment/slots", { body: body as EntertainmentSlotCreate })),
    onSettled: done,
  });
}
export function useDeleteSlot() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: (x: EntertainmentSlot) => call(api.DELETE("/api/v1/entertainment/slots/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })),
    onSettled: done,
  });
}
