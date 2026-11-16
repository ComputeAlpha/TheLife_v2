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
