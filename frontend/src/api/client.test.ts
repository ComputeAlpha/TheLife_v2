import { api, call } from "./client";
import { ApiError } from "./errors";
import { server, http, HttpResponse, problem } from "@/test/server";

describe("api client", () => {
  it("sends the CSRF header and an Idempotency-Key on writes, neither on reads", async () => {
    const seen: Record<string, Headers> = {};
    server.use(
      http.get("*/api/v1/me", ({ request }) => {
        seen.get = request.headers;
        return HttpResponse.json({});
      }),
      http.post("*/api/v1/tasks", ({ request }) => {
        seen.post = request.headers;
        return HttpResponse.json({}, { status: 201 });
      }),
    );
    await call(api.GET("/api/v1/me"));
    await call(api.POST("/api/v1/tasks", { body: { title: "x", area: "personal" } }));
    expect(seen.get!.get("X-TheLife-CSRF")).toBeNull();
    expect(seen.get!.get("Idempotency-Key")).toBeNull();
    expect(seen.post!.get("X-TheLife-CSRF")).toBe("1");
    expect(seen.post!.get("Idempotency-Key")).toMatch(/^[0-9a-f-]{36}$/);
  });

  it("turns problem+json into an ApiError with field errors", async () => {
    server.use(
      http.post("*/api/v1/me/password", () =>
        problem(422, "Validation failed", { errors: [{ path: "/current_password", message: "is incorrect" }] }),
      ),
    );
    const err = await call(api.POST("/api/v1/me/password", { body: { current_password: "a", new_password: "b".repeat(12) } })).catch((e) => e);
    expect(err).toBeInstanceOf(ApiError);
    expect(err.status).toBe(422);
    expect(err.requestId).toBe("req-1");
    expect(err.fieldErrors()).toEqual({ current_password: "is incorrect" });
  });

  it("reports rate limits with the wait", async () => {
    server.use(http.post("*/api/v1/auth/web-session", () => {
      const r = problem(429, "Too many requests");
      r.headers.set("Retry-After", "600");
      return r;
    }));
    const err = await call(api.POST("/api/v1/auth/web-session", { body: { email: "a@b.c", password: "x" } })).catch((e) => e);
    expect(err.summary).toBe("Too many attempts. Try again in 10 min.");
  });
});
