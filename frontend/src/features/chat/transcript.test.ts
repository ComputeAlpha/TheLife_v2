import type { ChatMessage } from "@/api/chat";
import { transcript } from "./transcript";

const m = (o: Partial<ChatMessage>): ChatMessage => ({ id: "x", conversation_id: "c", role: "user", kind: "normal", content: "", created_at: "2026-11-16T10:00:00Z", ...o });

describe("transcript", () => {
  it("leaves out steps that only loaded tool groups", () => {
    const t = transcript([
      m({ id: "1", role: "assistant", tool_calls: [{ id: "c0", name: "agent_load_tools", arguments: "{}" }] }),
      m({ id: "2", role: "tool", tool_call_id: "c0", content: "{}" }),
      m({ id: "3", role: "assistant", content: "Hi" }),
    ]);
    expect(t.map((e) => e.id)).toEqual(["3"]);
  });

  it("folds tool results into the step that called them and finds held approvals", () => {
    const t = transcript([
      m({ id: "1", role: "user", content: "Add a reading habit" }),
      m({ id: "2", role: "assistant", content: "", tool_calls: [{ id: "c1", name: "tasks_create", arguments: '{"title":"Read"}' }, { id: "c2", name: "tasks_delete", arguments: "{}" }] }),
      m({ id: "3", role: "tool", tool_call_id: "c1", content: '{"id":"t1"}' }),
      m({ id: "4", role: "tool", tool_call_id: "c2", content: '{"status":"pending","approval_id":"a1"}' }),
      m({ id: "5", role: "assistant", content: "Done." }),
    ]);
    expect(t.map((e) => e.id)).toEqual(["1", "2", "5"]);
    expect(t[1]!.steps).toMatchObject([
      { name: "tasks_create", result: '{"id":"t1"}', approvalId: null },
      { name: "tasks_delete", approvalId: "a1" },
    ]);
  });
});
