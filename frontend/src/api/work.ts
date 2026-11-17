// Work: roles, responsibilities, projects, deliverables and work contexts (frontend_lld.md §6.11).
import { useMutation, useQuery } from "@tanstack/react-query";
import { api, call, ifMatch, MERGE_PATCH } from "./client";
import { collect } from "./paging";
import { useInvalidate } from "./invalidate";
import type {
  WorkContext, WorkContextCreate, WorkDeliverable, WorkDeliverableCreate, WorkProject, WorkProjectCreate,
  WorkResponsibility, WorkResponsibilityCreate, WorkRole, WorkRoleCreate,
} from "./types";

const all = { limit: 200 } as const;
export function useRoles() {
  return useQuery({ queryKey: ["work", "roles"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/work/roles", { params: { query: { ...all, cursor } } }))) });
}
export function useResponsibilities() {
  return useQuery({ queryKey: ["work", "responsibilities"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/work/responsibilities", { params: { query: { ...all, cursor } } }))) });
}
export function useProjects() {
  return useQuery({ queryKey: ["work", "projects"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/work/projects", { params: { query: { ...all, cursor } } }))) });
}
export function useDeliverables() {
  return useQuery({ queryKey: ["work", "deliverables"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/work/deliverables", { params: { query: { ...all, cursor } } }))) });
}
export function useWorkContexts() {
  return useQuery({ queryKey: ["work", "contexts"], queryFn: () => collect((cursor) => call(api.GET("/api/v1/work-contexts", { params: { query: { ...all, cursor } } }))) });
}

const FAMILIES = ["work", "tasks", "progress", "scores"];
type Saver<T extends { id: string; version: number }, B> = { existing?: T; body: B };

export function useSaveRole() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: Saver<WorkRole, WorkRoleCreate>) =>
      existing
        ? call(api.PATCH("/api/v1/work/roles/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/work/roles", { body })),
    onSettled: done,
  });
}
export function useDeleteRole() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({ mutationFn: (x: WorkRole) => call(api.DELETE("/api/v1/work/roles/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}

export function useSaveResponsibility() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: Saver<WorkResponsibility, WorkResponsibilityCreate>) =>
      existing
        ? call(api.PATCH("/api/v1/work/responsibilities/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/work/responsibilities", { body })),
    onSettled: done,
  });
}
export function useDeleteResponsibility() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({ mutationFn: (x: WorkResponsibility) => call(api.DELETE("/api/v1/work/responsibilities/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}

export function useSaveProject() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: Saver<WorkProject, WorkProjectCreate>) =>
      existing
        ? call(api.PATCH("/api/v1/work/projects/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/work/projects", { body })),
    onSettled: done,
  });
}
export function useDeleteProject() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({ mutationFn: (x: WorkProject) => call(api.DELETE("/api/v1/work/projects/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}

export function useSaveDeliverable() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: Saver<WorkDeliverable, WorkDeliverableCreate>) =>
      existing
        ? call(api.PATCH("/api/v1/work/deliverables/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/work/deliverables", { body })),
    onSettled: done,
  });
}
export function useDeleteDeliverable() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({ mutationFn: (x: WorkDeliverable) => call(api.DELETE("/api/v1/work/deliverables/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}

export function useSaveWorkContext() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({
    mutationFn: ({ existing, body }: Saver<WorkContext, WorkContextCreate>) =>
      existing
        ? call(api.PATCH("/api/v1/work-contexts/{id}", { params: { path: { id: existing.id }, header: ifMatch(existing.version) }, body, headers: MERGE_PATCH }))
        : call(api.POST("/api/v1/work-contexts", { body })),
    onSettled: done,
  });
}
export function useDeleteWorkContext() {
  const done = useInvalidate(...FAMILIES);
  return useMutation({ mutationFn: (x: WorkContext) => call(api.DELETE("/api/v1/work-contexts/{id}", { params: { path: { id: x.id }, header: ifMatch(x.version) } })), onSettled: done });
}
