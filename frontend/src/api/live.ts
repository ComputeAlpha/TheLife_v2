// The live layer of a chat (frontend_lld.md §5): what the SSE stream says about turns in flight. Everything that is
// persisted comes from the server's message list, refetched on `message`, `tool.finished` and `turn.finished`; this
// state only carries what isn't stored yet: streamed text, the tool running now, and the turn's status.
export interface LiveTurn {
  turnId: string;
  conversationId?: string;
  text: string;
  /** Streamed text was retracted (a retry, or leaked reasoning) and nothing new has arrived yet. */
  reset: boolean;
  running: string[];
  status: "running" | "done" | "failed" | "manual" | "merged";
  messageId?: string | null;
}

export type StreamEvent =
  | { type: "turn.started"; data: { turn_id: string; conversation_id?: string } }
  | { type: "token"; data: { turn_id: string; text: string } }
  | { type: "reset"; data: { turn_id: string } }
  | { type: "tool.started"; data: { turn_id: string; tool: string } }
  | { type: "tool.finished"; data: { turn_id: string; tool: string; outcome: string } }
  | { type: "approval.requested"; data: { turn_id: string; approval_id: string; tool: string } }
  | { type: "message"; data: { turn_id?: string; message_id: string; kind: string; text: string } }
  | { type: "turn.finished"; data: { turn_id: string; status: LiveTurn["status"]; message_id?: string | null } }
  | { type: "notification"; data: { id: string; kind: string; urgency: string; title: string; body: string } };

export type LiveState = Record<string, LiveTurn>;

function turn(s: LiveState, id: string): LiveTurn {
  return s[id] ?? { turnId: id, text: "", reset: false, running: [], status: "running" };
}

export function liveReducer(s: LiveState, e: StreamEvent): LiveState {
  switch (e.type) {
    case "turn.started": {
      const t = turn(s, e.data.turn_id);
      return { ...s, [t.turnId]: { ...t, conversationId: e.data.conversation_id, status: "running" } };
    }
    case "token": {
      const t = turn(s, e.data.turn_id);
      return { ...s, [t.turnId]: { ...t, text: t.text + e.data.text, reset: false } };
    }
    case "reset": {
      const t = turn(s, e.data.turn_id);
      return { ...s, [t.turnId]: { ...t, text: "", reset: true } };
    }
    case "tool.started": {
      const t = turn(s, e.data.turn_id);
      return { ...s, [t.turnId]: { ...t, running: [...t.running, e.data.tool] } };
    }
    case "tool.finished": {
      const t = turn(s, e.data.turn_id);
      const i = t.running.indexOf(e.data.tool);
      return { ...s, [t.turnId]: { ...t, running: i < 0 ? t.running : t.running.filter((_, k) => k !== i) } };
    }
    case "message": {
      // A step was stored (its text now comes from the server list), so the streamed copy goes.
      if (!e.data.turn_id || !s[e.data.turn_id]) return s;
      const t = s[e.data.turn_id]!;
      return { ...s, [t.turnId]: { ...t, text: "", reset: false } };
    }
    case "turn.finished": {
      const t = turn(s, e.data.turn_id);
      return { ...s, [t.turnId]: { ...t, status: e.data.status, messageId: e.data.message_id ?? null, running: [], text: "", reset: false } };
    }
    default:
      return s;
  }
}

/** Drops finished turns once the server list holds their reply (or they had none). */
export function pruneLive(s: LiveState, storedIds: Set<string>): LiveState {
  let changed = false;
  const out: LiveState = {};
  for (const [k, t] of Object.entries(s)) {
    if (t.status !== "running" && (!t.messageId || storedIds.has(t.messageId))) {
      changed = true;
      continue;
    }
    out[k] = t;
  }
  return changed ? out : s;
}

/** "tasks_create" / "tasks.create" → "Create task". */
export function toolLabel(name: string): string {
  const parts = name.split(/[._]/).filter(Boolean);
  if (parts.length < 2) return name.replace(/[._]/g, " ");
  const verb = parts.at(-1)!;
  const noun = parts.slice(0, -1).join(" ");
  const single = noun.endsWith("ies") ? noun.slice(0, -3) + "y" : noun.endsWith("s") && !noun.endsWith("ss") ? noun.slice(0, -1) : noun;
  return `${verb[0]!.toUpperCase()}${verb.slice(1).replace(/_/g, " ")} ${single}`;
}
