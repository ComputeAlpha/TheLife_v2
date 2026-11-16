import { screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { LoginPage, safeNext } from "./LoginPage";
import { renderApp, Route } from "@/test/render";
import { server, http, HttpResponse, problem, testUser } from "@/test/server";

describe("safeNext", () => {
  it("only follows same-origin paths", () => {
    expect(safeNext("/calendar?x=1")).toBe("/calendar?x=1");
    expect(safeNext("//evil.example")).toBe("/");
    expect(safeNext("/\\evil.example")).toBe("/");
    expect(safeNext("https://evil.example")).toBe("/");
    expect(safeNext(null)).toBe("/");
  });
});

describe("LoginPage", () => {
  beforeEach(() => server.use(http.get("*/api/v1/me", () => problem(401, "Unauthorized"))));

  const routes = (
    <>
      <Route path="/login" element={<LoginPage />} />
      <Route path="/chat" element={<p>chat page</p>} />
    </>
  );

  it("signs in and goes to the next page", async () => {
    let body: unknown;
    server.use(
      http.post("*/api/v1/auth/web-session", async ({ request }) => {
        body = await request.json();
        return HttpResponse.json({ expires_at: "2026-12-16T00:00:00Z", user: testUser });
      }),
    );
    renderApp(null, { path: "/login?next=%2Fchat", routes });
    await userEvent.type(await screen.findByLabelText("Email"), " asha@example.com ");
    await userEvent.type(screen.getByLabelText("Password"), "correct horse battery");
    await userEvent.click(screen.getByRole("button", { name: /sign in/i }));
    expect(await screen.findByText("chat page")).toBeInTheDocument();
    expect(body).toEqual({ email: "asha@example.com", password: "correct horse battery" });
  });

  it("explains a wrong password without saying which part was wrong", async () => {
    server.use(http.post("*/api/v1/auth/web-session", () => problem(401, "Unauthorized", { detail: "Invalid email or password." })));
    renderApp(null, { path: "/login", routes });
    await userEvent.type(await screen.findByLabelText("Email"), "asha@example.com");
    await userEvent.type(screen.getByLabelText("Password"), "nope");
    await userEvent.click(screen.getByRole("button", { name: /sign in/i }));
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent("That email and password don't match."));
  });
});
