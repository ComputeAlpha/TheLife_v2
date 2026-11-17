import type { ChatMessage } from "@/api/chat";

export interface Step {
  id: string;
  name: string;
  arguments: string;
  result: string | null;
  /** Set when the call was held for approval. */
  approvalId: string | null;
  failed: boolean;
}
export interface Entry {
  id: string;
  role: "user" | "assistant";
  kind: ChatMessage["kind"];
  content: string;
  createdAt: string;
  turnId: string | null;
  steps: Step[];
}

/** Messages as display entries: each assistant step's tool calls and their results folded into it. */
export function transcript(messages: ChatMessage[]): Entry[] {
  const out: Entry[] = [];
  const byCall = new Map<string, Step>();
  for (const m of messages) {
    if (m.role === "tool") {
      const s = byCall.get(m.tool_call_id ?? "");
      if (s) {
        s.result = m.content;
        try {
          const r = JSON.parse(m.content);
          if (r && typeof r === "object") {
            if (typeof r.approval_id === "string") s.approvalId = r.approval_id;
            if (typeof r.error === "string" || r.status === "failed") s.failed = true;
          }
        } catch {
          /* plain text result */
        }
      }
      continue;
    }
    const e: Entry = { id: m.id, role: m.role, kind: m.kind, content: m.content, createdAt: m.created_at, turnId: m.turn_id ?? null, steps: [] };
    for (const c of (m.tool_calls ?? []).filter((c) => !HIDDEN_TOOLS.has(c.name))) {
      const s: Step = { id: c.id, name: c.name, arguments: c.arguments, result: null, approvalId: null, failed: false };
      byCall.set(c.id, s);
      e.steps.push(s);
    }
    // A step that only loaded tool groups says nothing to the user.
    if (e.role === "assistant" && !e.content && !e.steps.length && (m.tool_calls ?? []).length) continue;
    out.push(e);
  }
  return out;
}

/** Internal plumbing calls the transcript leaves out. */
export const HIDDEN_TOOLS = new Set(["agent_load_tools", "agent.load_tools"]);

export function pretty(json: string | null): string {
  if (json == null) return "";
  try {
    return JSON.stringify(JSON.parse(json), null, 2);
  } catch {
    return json;
  }
}
