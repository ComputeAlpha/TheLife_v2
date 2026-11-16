// RFC 9457 problem details from the backend, as a typed error (frontend_lld.md §5).
export interface FieldProblem {
  path: string;
  message: string;
}

export class ApiError extends Error {
  readonly status: number;
  readonly type: string;
  readonly title: string;
  readonly detail?: string;
  readonly requestId?: string;
  readonly errors: FieldProblem[];
  readonly retryAfterS?: number;

  constructor(status: number, body: unknown, retryAfter?: string | null) {
    const p = (body && typeof body === "object" ? body : {}) as Record<string, unknown>;
    const title = typeof p.title === "string" ? p.title : status === 0 ? "Network error" : `HTTP ${status}`;
    const detail = typeof p.detail === "string" ? p.detail : undefined;
    super(detail ?? title);
    this.name = "ApiError";
    this.status = status;
    this.type = typeof p.type === "string" ? p.type : "about:blank";
    this.title = title;
    this.detail = detail;
    this.requestId = typeof p.request_id === "string" ? p.request_id : undefined;
    this.errors = Array.isArray(p.errors)
      ? p.errors.filter((e): e is FieldProblem => !!e && typeof e.path === "string" && typeof e.message === "string")
      : [];
    const ra = retryAfter ? Number(retryAfter) : NaN;
    this.retryAfterS = Number.isFinite(ra) ? ra : undefined;
  }

  /** Field errors keyed by the last JSON Pointer segment ("/new_password" → "new_password"). */
  fieldErrors(): Record<string, string> {
    const out: Record<string, string> = {};
    for (const e of this.errors) {
      const key = e.path.replace(/^\//, "").split("/")[0] || "_";
      out[key] ??= e.message;
    }
    return out;
  }

  /** One line for a toast or banner. */
  get summary(): string {
    if (this.status === 0) return "Can't reach the server. Check that the backend is running.";
    if (this.status === 429)
      return this.retryAfterS ? `Too many attempts. Try again in ${Math.ceil(this.retryAfterS / 60)} min.` : this.title;
    if (this.errors.length && !this.detail) return this.errors.map((e) => `${e.path || "request"}: ${e.message}`).join("; ");
    return this.detail ?? this.title;
  }
}

export function isApiError(e: unknown, status?: number): e is ApiError {
  return e instanceof ApiError && (status === undefined || e.status === status);
}
