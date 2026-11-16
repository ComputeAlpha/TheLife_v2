// One SSE connection per tab (frontend_lld.md §5). It feeds the chat's live layer, keeps pages fresh when the
// assistant changes data, and turns `notification` events into toasts (and browser notifications when the tab is
// hidden). The session cookie authenticates it; no token is handled here.
import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { useQueryClient, type QueryClient } from "@tanstack/react-query";
import { useNavigate } from "react-router";
import type { StreamEvent } from "./live";
import { ackNotification, notificationHref } from "./notifications";
import { toast } from "@/ui/toast";

type Listener = (e: StreamEvent) => void;
interface StreamCtx {
  subscribe: (fn: Listener) => () => void;
  connected: boolean;
}
const Ctx = createContext<StreamCtx>({ subscribe: () => () => {}, connected: false });
export const useStream = () => useContext(Ctx);

const EVENTS = ["turn.started", "token", "reset", "tool.started", "tool.finished", "approval.requested", "message", "turn.finished", "notification"] as const;

/** Query families a tool can change, by the tool's resource prefix. */
function familiesFor(tool: string): string[] {
  const prefix = tool.split(/[._]/)[0] ?? "";
  const base = ["calendar", "progress"];
  const map: Record<string, string[]> = {
    tasks: ["tasks"], events: ["events"], calendar: ["events"], detections: ["detections"], memory: ["memory"],
    reminders: ["reminders"], routines: ["routines"], goals: ["goals"], plans: ["plans"], targets: ["targets"],
    skills: ["skill-resources", "sessions"], entertainment: ["entertainment-items", "slots"], scores: ["scores"],
    work: ["work"], activities: ["activities"], day: ["overrides", "day-profiles"],
  };
  return [...base, ...(map[prefix] ?? [])];
}

function refreshAfter(qc: QueryClient, e: StreamEvent) {
  switch (e.type) {
    case "message":
    case "turn.finished":
      qc.invalidateQueries({ queryKey: ["messages"] });
      qc.invalidateQueries({ queryKey: ["conversations"] });
      break;
    case "tool.finished":
      qc.invalidateQueries({ queryKey: ["messages"] });
      if (e.data.outcome === "executed") for (const k of familiesFor(e.data.tool)) qc.invalidateQueries({ queryKey: [k] });
      break;
    case "approval.requested":
      qc.invalidateQueries({ queryKey: ["approvals"] });
      break;
    case "turn.started":
      qc.invalidateQueries({ queryKey: ["conversations"] });
      break;
    case "notification":
      qc.invalidateQueries({ queryKey: ["notifications"] });
      break;
  }
}

export function StreamProvider({ children }: { children: ReactNode }) {
  const qc = useQueryClient();
  const navigate = useNavigate();
  const listeners = useRef(new Set<Listener>());
  const [connected, setConnected] = useState(false);
  const nav = useRef(navigate);
  useEffect(() => {
    nav.current = navigate;
  }, [navigate]);

  useEffect(() => {
    let es: EventSource | null = null;
    let retry = 1000;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let wasDown = false;
    let stopped = false;

    const onNotification = (n: Extract<StreamEvent, { type: "notification" }>["data"]) => {
      const go = () => {
        void ackNotification(n.id, true).catch(() => {});
        nav.current(notificationHref({}));
      };
      if (document.visibilityState === "visible") {
        toast(n.title, { body: n.body, tone: n.urgency === "critical" ? "alert" : "info", action: { label: "Open", onClick: go } });
        // Seen here, so Telegram needn't carry it too.
        void ackNotification(n.id, false).catch(() => {});
      } else if ("Notification" in window && Notification.permission === "granted") {
        const b = new Notification(n.title, { body: n.body, tag: n.id, icon: "/favicon.svg" });
        b.onclick = () => {
          window.focus();
          go();
          b.close();
        };
      }
      // Otherwise the server's routing falls through to Telegram after its wait.
    };

    const connect = () => {
      if (stopped) return;
      es = new EventSource("/api/v1/chat/web/stream");
      es.onopen = () => {
        setConnected(true);
        retry = 1000;
        if (wasDown) {
          // Events may have been missed (the stream has no ids): refresh what they would have touched.
          for (const k of ["messages", "conversations", "approvals", "notifications"]) qc.invalidateQueries({ queryKey: [k] });
          wasDown = false;
        }
      };
      es.onerror = () => {
        setConnected(false);
        wasDown = true;
        if (es?.readyState === EventSource.CLOSED) {
          // The browser gave up (an HTTP error such as 401): retry with backoff; the auth gate handles sign-out.
          es.close();
          timer = setTimeout(connect, retry);
          retry = Math.min(30_000, retry * 2);
        }
      };
      for (const name of EVENTS) {
        es.addEventListener(name, (m) => {
          let data: unknown;
          try {
            data = JSON.parse((m as MessageEvent).data);
          } catch {
            return;
          }
          const e = { type: name, data } as StreamEvent;
          refreshAfter(qc, e);
          if (e.type === "notification") onNotification(e.data);
          listeners.current.forEach((fn) => fn(e));
        });
      }
    };
    connect();
    return () => {
      stopped = true;
      clearTimeout(timer);
      es?.close();
    };
  }, [qc]);

  return (
    <Ctx.Provider
      value={{
        connected,
        subscribe: (fn) => {
          listeners.current.add(fn);
          return () => listeners.current.delete(fn);
        },
      }}
    >
      {children}
    </Ctx.Provider>
  );
}
