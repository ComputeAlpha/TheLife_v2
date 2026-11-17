// Title search for the command palette (frontend_lld.md §4): tasks, goals, books and courses, and events.
import { useQuery } from "@tanstack/react-query";
import { api, call } from "./client";
import { collect } from "./paging";

export interface Hit { kind: "task" | "goal" | "resource" | "event"; id: string; title: string; to: string; hint?: string }

/** Loaded only while the palette is open; cached for a minute. */
export function useSearchIndex(enabled: boolean, eventDay: (iso: string) => string) {
  return useQuery({
    queryKey: ["search-index"],
    enabled,
    staleTime: 60_000,
    queryFn: async (): Promise<Hit[]> => {
      const now = Date.now();
      const [tasks, goals, resources, events] = await Promise.all([
        collect((cursor) => call(api.GET("/api/v1/tasks", { params: { query: { status: "open", limit: 200, cursor } } })), 3),
        collect((cursor) => call(api.GET("/api/v1/goals", { params: { query: { limit: 200, cursor } } })), 2),
        collect((cursor) => call(api.GET("/api/v1/skills/resources", { params: { query: { limit: 200, cursor } } })), 2),
        collect((cursor) => call(api.GET("/api/v1/events", { params: { query: { from: new Date(now - 30 * 86_400_000).toISOString(), to: new Date(now + 180 * 86_400_000).toISOString(), limit: 200, cursor } } })), 3),
      ]);
      return [
        ...tasks.map((t): Hit => ({ kind: "task", id: t.id, title: t.title, to: `/tasks?task=${t.id}` })),
        ...goals.map((g): Hit => ({ kind: "goal", id: g.id, title: g.title, to: `/plan?goal=${g.id}`, hint: g.horizon })),
        ...resources.map((r): Hit => ({ kind: "resource", id: r.id, title: r.title, to: `/skills/${r.id}`, hint: r.kind })),
        ...events.map((e): Hit => ({ kind: "event", id: e.id, title: e.title, to: `/calendar/day/${eventDay(e.starts_at)}`, hint: e.rrule ? "repeats" : undefined })),
      ];
    },
  });
}
