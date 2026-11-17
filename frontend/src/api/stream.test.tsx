import { act, screen, waitFor } from "@testing-library/react";
import { StreamProvider, useStream } from "./stream";
import { renderApp } from "@/test/render";
import { Toaster } from "@/ui/toast";
import { server, http, HttpResponse } from "@/test/server";
import type { StreamEvent } from "./live";
import { useEffect } from "react";

class FakeEventSource {
  static last: FakeEventSource | null = null;
  static CLOSED = 2;
  readyState = 1;
  onopen: (() => void) | null = null;
  onerror: (() => void) | null = null;
  url: string;
  private handlers = new Map<string, ((m: MessageEvent) => void)[]>();
  constructor(url: string) {
    this.url = url;
    FakeEventSource.last = this;
  }
  addEventListener(name: string, fn: (m: MessageEvent) => void) {
    this.handlers.set(name, [...(this.handlers.get(name) ?? []), fn]);
  }
  emit(name: string, data: unknown) {
    for (const fn of this.handlers.get(name) ?? []) fn(new MessageEvent(name, { data: JSON.stringify(data) }));
  }
  close() {
    this.readyState = 2;
  }
}

describe("StreamProvider", () => {
  beforeEach(() => {
    vi.stubGlobal("EventSource", FakeEventSource);
  });
  afterEach(() => vi.unstubAllGlobals());

  it("opens the user's stream and turns a notification into a toast, acked as seen", async () => {
    const acks: unknown[] = [];
    server.use(
      http.post("*/api/v1/notifications/:id", async ({ request, params }) => {
        acks.push({ id: params.id, body: await request.json() });
        return new HttpResponse(null, { status: 204 });
      }),
    );
    renderApp(
      <StreamProvider>
        <Toaster />
      </StreamProvider>,
    );
    expect(FakeEventSource.last!.url).toBe("/api/v1/chat/web/stream");
    act(() => FakeEventSource.last!.emit("notification", { id: "n1", kind: "reminder", urgency: "normal", title: "Reminder", body: "Call mom" }));
    expect(await screen.findByText("Call mom")).toBeInTheDocument();
    await waitFor(() => expect(acks).toEqual([{ id: "n1:ack", body: { clicked: false } }]));
  });

  it("hands chat events to subscribers", () => {
    const seen: StreamEvent[] = [];
    function Probe() {
      const s = useStream();
      useEffect(() => s.subscribe((e) => seen.push(e)), [s]);
      return null;
    }
    renderApp(
      <StreamProvider>
        <Probe />
      </StreamProvider>,
    );
    act(() => FakeEventSource.last!.emit("token", { turn_id: "t", text: "Hi" }));
    expect(seen).toEqual([{ type: "token", data: { turn_id: "t", text: "Hi" } }]);
  });
});
