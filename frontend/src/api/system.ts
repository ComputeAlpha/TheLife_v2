import { useQuery } from "@tanstack/react-query";
import { api, call } from "./client";

export function useLlmStatus() {
  return useQuery({
    queryKey: ["system", "llm-status"],
    queryFn: () => call(api.GET("/api/v1/system/llm-status")),
    refetchInterval: 60_000,
    staleTime: 30_000,
  });
}

/** Server version (liveness) and readiness, for Settings → About. */
export function useServerInfo() {
  return useQuery({
    queryKey: ["system", "info"],
    queryFn: async () => {
      const [health, ready] = await Promise.all([call(api.GET("/healthz")), api.GET("/readyz").then((r) => r.data ?? (r.error as unknown) ?? null)]);
      return { version: health.version, ready: ready as { status?: string; checks?: Record<string, unknown>; schema_version?: string | number } | null };
    },
  });
}
