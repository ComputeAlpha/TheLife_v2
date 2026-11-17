// Settings → Connections, Tracking, Days and scoring (frontend_lld.md §6.14).
import { useMutation, useQuery } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import { useInvalidate } from "./invalidate";
import type { ApiKey, DayProfile, DayProfileCreate, DayProfileOverride, Device, IngestRule, IngestRuleCreate, Scope, ScoringSettings } from "./types";

// ---- Telegram
export function useTelegramLink() {
  return useQuery({ queryKey: ["telegram"], queryFn: () => call(api.GET("/api/v1/telegram/link")) });
}
export function useTelegramCode() {
  return useMutation({ mutationFn: () => call(api.POST("/api/v1/telegram/link-code")) });
}
export function useTelegramUnlink() {
  const done = useInvalidate("telegram");
  return useMutation({ mutationFn: () => call(api.DELETE("/api/v1/telegram/link")), onSettled: done });
}

// ---- Devices
export function useDeviceList() {
  return useQuery({
    queryKey: ["devices", "all"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/devices", { params: { query: { limit: 200, cursor } } }))),
    refetchInterval: 60_000,
  });
}
export function useRegisterDevice() {
  const done = useInvalidate("devices");
  return useMutation({ mutationFn: (name: string) => call(api.POST("/api/v1/devices", { body: { name, platform: "linux-x11" } })), onSettled: done });
}
export function useRevokeDevice() {
  const done = useInvalidate("devices");
  return useMutation({ mutationFn: (d: Device) => call(api.DELETE("/api/v1/devices/{id}", { params: { path: { id: d.id } } })), onSettled: done });
}

// ---- API keys
export function useApiKeys() {
  return useQuery({ queryKey: ["api-keys"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/api-keys", { params: { query: { limit: 200, cursor } } }))) });
}
export function useIssueApiKey() {
  const done = useInvalidate("api-keys");
  return useMutation({
    mutationFn: (body: { name: string; scopes: Scope[]; expires_at?: string | null }) => call(api.POST("/api/v1/api-keys", { body })),
    onSettled: done,
  });
}
export function useRevokeApiKey() {
  const done = useInvalidate("api-keys");
  return useMutation({ mutationFn: (k: ApiKey) => call(api.DELETE("/api/v1/api-keys/{id}", { params: { path: { id: k.id } } })), onSettled: done });
}

// ---- Ingest rules
export function useIngestRules() {
  return useQuery({ queryKey: ["ingest-rules"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/ingest/rules", { params: { query: { limit: 200, cursor } } }))) });
}
export function useSaveIngestRule() {
  const done = useInvalidate("ingest-rules");
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: IngestRule; body: Partial<IngestRuleCreate> }) =>
      existing
        ? call(api.PATCH("/api/v1/ingest/rules/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/ingest/rules", { body: body as IngestRuleCreate })),
    onSettled: done,
  });
}
export function useDeleteIngestRule() {
  const done = useInvalidate("ingest-rules");
  return useMutation({ mutationFn: (r: IngestRule) => call(api.DELETE("/api/v1/ingest/rules/{id}", { params: { path: { id: r.id }, header: ifMatch(r.version) } })), onSettled: done });
}

// ---- Day profiles and overrides
export function useSaveDayProfile() {
  const done = useInvalidate("day-profiles", "progress", "scores");
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: DayProfile; body: DayProfileCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/day-profiles/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/day-profiles", { body })),
    onSettled: done,
  });
}
export function useDeleteDayProfile() {
  const done = useInvalidate("day-profiles");
  return useMutation({ mutationFn: (p: DayProfile) => call(api.DELETE("/api/v1/day-profiles/{id}", { params: { path: { id: p.id }, header: ifMatch(p.version) } })), onSettled: done });
}
/** Changes an existing override's profile in place (the calendar's day menu deletes and recreates instead). */
export function useChangeOverride() {
  const done = useInvalidate("overrides", "progress", "calendar");
  return useMutation({
    mutationFn: ({ o, profile }: { o: DayProfileOverride; profile: string }) =>
      call(api.PATCH("/api/v1/day-profile-overrides/{id}", { params: { path: { id: o.id }, header: ifMatch(o.version) }, body: { profile }, headers: MERGE_PATCH })),
    onSettled: done,
  });
}

// ---- Scoring settings
export function useScoringSettings() {
  return useQuery({ queryKey: ["scoring-settings"], queryFn: () => call(api.GET("/api/v1/settings/scoring")) });
}
export function useSaveScoringSettings() {
  const done = useInvalidate("scoring-settings", "scores");
  return useMutation({
    mutationFn: (body: ScoringSettings) => call(api.PATCH("/api/v1/settings/scoring", { body, headers: MERGE_PATCH })),
    onSettled: done,
  });
}
