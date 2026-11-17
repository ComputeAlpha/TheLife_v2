// The typed API client (frontend_lld.md §5). Types come from the contract (src/api/schema.d.ts, generated).
// The session lives in an HttpOnly cookie; scripts never see a token. Unsafe methods carry the CSRF header that
// cookie-authenticated requests need, and creates carry an Idempotency-Key so a retried click never duplicates.
import createClient, { type Middleware } from "openapi-fetch";
import type { paths } from "./schema";
import { ApiError } from "./errors";

type Listener = () => void;
const unauthorizedListeners = new Set<Listener>();
/** Called on any 401 outside sign-in, so the app can drop its cache and show the login page. */
export function onUnauthorized(fn: Listener): () => void {
  unauthorizedListeners.add(fn);
  return () => unauthorizedListeners.delete(fn);
}

export function newId(): string {
  return crypto.randomUUID();
}

/**
 * POSTs whose contract declares no Idempotency-Key, chiefly those that issue a secret (a replayed response would
 * hand the token out again). `npm run check:api` checks this list against the contract.
 */
export const NO_IDEMPOTENCY_KEY = new Set([
  "/api/v1/api-keys",
  "/api/v1/auth/login",
  "/api/v1/devices",
  "/api/v1/devices/self/token:rotate",
  "/api/v1/ingest/captures", // device-only; listed so the set matches the contract
  "/api/v1/ingest/observations",
]);

const middleware: Middleware = {
  onRequest({ request }) {
    if (request.method !== "GET" && request.method !== "HEAD") {
      request.headers.set("X-TheLife-CSRF", "1");
      if (request.method === "POST" && !request.headers.has("Idempotency-Key") && !NO_IDEMPOTENCY_KEY.has(new URL(request.url).pathname)) {
        request.headers.set("Idempotency-Key", newId());
      }
    }
    return request;
  },
  onResponse({ request, response }) {
    const path = new URL(request.url).pathname;
    if (response.status === 401 && !path.startsWith("/api/v1/auth/") && path !== "/api/v1/me") {
      unauthorizedListeners.forEach((fn) => fn());
    }
    return response;
  },
};

export const api = createClient<paths>({
  baseUrl: typeof location === "undefined" ? "" : location.origin,
  credentials: "same-origin",
  // Looked up per call, so test interceptors (MSW) installed after this module loads still apply.
  fetch: (req: Request) => globalThis.fetch(req),
});
api.use(middleware);

type Result<T> = { data?: T; error?: unknown; response: Response };

/** Unwraps an openapi-fetch result: the data, or a thrown ApiError (status 0 for network failures). */
export async function call<T>(p: Promise<Result<T>>): Promise<T> {
  let r: Result<T>;
  try {
    r = await p;
  } catch (e) {
    if (e instanceof ApiError) throw e;
    throw new ApiError(0, { title: "Network error", detail: String(e) });
  }
  if (r.response.ok) return r.data as T;
  throw new ApiError(r.response.status, r.error, r.response.headers.get("Retry-After"));
}

/** If-Match for optimistic concurrency: resources carry `version`, sent back quoted. */
export function ifMatch(version: number): { "If-Match": string } {
  return { "If-Match": `"${version}"` };
}

export const MERGE_PATCH = { "Content-Type": "application/merge-patch+json" } as const;
