import { useMutation, useQuery, useQueryClient, type QueryClient } from "@tanstack/react-query";
import { api, call, MERGE_PATCH } from "./client";
import type { User } from "./types";
import { isApiError } from "./errors";

export const meKey = ["me"] as const;

/**
 * Signs the tab out locally: `me` becomes null (the auth gate shows the login page) and every other cached query
 * is dropped. The `me` query itself is kept, so its observers see the change (QueryClient.clear() would orphan them).
 */
export function dropSession(qc: QueryClient) {
  qc.setQueryData(meKey, null);
  qc.removeQueries({ predicate: (q) => q.queryKey[0] !== meKey[0] });
}

/** The signed-in user, or null when signed out (401). */
export function useMe() {
  return useQuery({
    queryKey: meKey,
    queryFn: async (): Promise<User | null> => {
      try {
        return await call(api.GET("/api/v1/me"));
      } catch (e) {
        if (isApiError(e, 401)) return null;
        throw e;
      }
    },
    staleTime: 5 * 60_000,
    retry: (n, e) => !isApiError(e) && n < 2,
  });
}

/** The signed-in user; only for components under the auth gate. */
export function useUser(): User {
  const { data } = useMe();
  if (!data) throw new Error("useUser outside the signed-in area");
  return data;
}

export function useSignIn() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: { email: string; password: string }) => call(api.POST("/api/v1/auth/web-session", { body })),
    onSuccess: (s) => {
      qc.removeQueries({ predicate: (q) => q.queryKey[0] !== meKey[0] });
      qc.setQueryData(meKey, s.user);
    },
  });
}

export function useSignOut() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => call(api.DELETE("/api/v1/auth/web-session")),
    onSettled: () => dropSession(qc),
  });
}

export type ProfilePatch = Partial<Pick<User, "display_name" | "timezone" | "day_start_hour" | "workdays">>;

export function useUpdateProfile() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: ProfilePatch) => call(api.PATCH("/api/v1/me", { body, headers: MERGE_PATCH })),
    onSuccess: (u) => {
      qc.setQueryData(meKey, u);
      // Every date on screen depends on the timezone and day start.
      qc.invalidateQueries({ predicate: (q) => q.queryKey[0] !== "me" });
    },
  });
}

export function useChangePassword() {
  return useMutation({
    mutationFn: (body: { current_password: string; new_password: string }) =>
      call(api.POST("/api/v1/me/password", { body })),
  });
}
