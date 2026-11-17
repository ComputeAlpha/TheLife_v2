// Skills: books and courses, their sections and study sessions (frontend_lld.md §6.10).
import { useMutation, useQuery } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import { useInvalidate } from "./invalidate";
import type { CourseSection, SkillResource, SkillResourceCreate, SkillSession, SkillSessionCreate } from "./types";

export function useResources() {
  return useQuery({
    queryKey: ["skill-resources", "all"],
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/skills/resources", { params: { query: { limit: 200, cursor } } }))),
  });
}
export function useSections(resourceId: string | null) {
  return useQuery({
    queryKey: ["skill-sections", resourceId],
    enabled: !!resourceId,
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/skills/sections", { params: { query: { resource_id: resourceId!, limit: 200, cursor } } }))),
  });
}
export function useSessions(resourceId: string | null) {
  return useQuery({
    queryKey: ["sessions", resourceId],
    enabled: !!resourceId,
    queryFn: () => collect((cursor) => call(api.GET("/api/v1/skills/sessions", { params: { query: { resource_id: resourceId!, limit: 200, cursor } } })), 3),
  });
}

const FAMILIES = ["skill-resources", "skill-sections", "sessions", "calendar", "progress", "activities"];

export function useSaveResource() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: SkillResource; body: SkillResourceCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/skills/resources/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/skills/resources", { body })),
    onSettled: done,
  });
}
export function useDeleteResource() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: (r: SkillResource) => call(api.DELETE("/api/v1/skills/resources/{id}", { params: { path: { id: r.id }, header: ifMatch(r.version) } })),
    onSettled: done,
  });
}

export function useSaveSection() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: CourseSection; body: { resource_id: string; ordinal: number; title: string; duration_s?: number | null } }) =>
      existing
        ? call(api.PATCH("/api/v1/skills/sections/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body: { ordinal: body.ordinal, title: body.title, duration_s: body.duration_s }, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/skills/sections", { body })),
    onSettled: done,
  });
}
export function useDeleteSection() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: (s: CourseSection) => call(api.DELETE("/api/v1/skills/sections/{id}", { params: { path: { id: s.id }, header: ifMatch(s.version) } })),
    onSettled: done,
  });
}

export function useSaveSession() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: { existing?: SkillSession; body: SkillSessionCreate }) =>
      existing
        ? call(api.PATCH("/api/v1/skills/sessions/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/skills/sessions", { body })),
    onSettled: done,
  });
}
export function useDeleteSession() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: (s: SkillSession) => call(api.DELETE("/api/v1/skills/sessions/{id}", { params: { path: { id: s.id }, header: ifMatch(s.version) } })),
    onSettled: done,
  });
}
