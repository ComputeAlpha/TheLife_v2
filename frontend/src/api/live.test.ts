import { liveReducer, pruneLive, toolLabel, type LiveState, type StreamEvent } from "./live";

const run = (events: StreamEvent[]) => events.reduce(liveReducer, {} as LiveState);

describe("live chat turns", () => {
  it("streams tokens, tracks running tools and finishes", () => {
    const s = run([
      { type: "turn.started", data: { turn_id: "t", conversation_id: "c" } },
      { type: "token", data: { turn_id: "t", text: "Plan" } },
      { type: "token", data: { turn_id: "t", text: "ning…" } },
      { type: "message", data: { turn_id: "t", message_id: "m1", kind: "normal", text: "Planning…" } },
      { type: "tool.started", data: { turn_id: "t", tool: "tasks_create" } },
    ]);
    expect(s.t).toMatchObject({ conversationId: "c", text: "", running: ["tasks_create"], status: "running" });
    const done = ([
      { type: "tool.finished", data: { turn_id: "t", tool: "tasks_create", outcome: "executed" } },
      { type: "token", data: { turn_id: "t", text: "Done!" } },
      { type: "turn.finished", data: { turn_id: "t", status: "done", message_id: "m2" } },
    ] as StreamEvent[]).reduce(liveReducer, s as LiveState);
    expect(done.t).toMatchObject({ running: [], status: "done", messageId: "m2", text: "" });
  });

  it("retracts streamed text on reset", () => {
    const s = run([
      { type: "token", data: { turn_id: "t", text: "<think>leaked" } },
      { type: "reset", data: { turn_id: "t" } },
    ]);
    expect(s.t).toMatchObject({ text: "", reset: true });
    expect(liveReducer(s, { type: "token", data: { turn_id: "t", text: "Hi" } }).t).toMatchObject({ text: "Hi", reset: false });
  });

  it("keeps a finished turn until its reply is in the stored list", () => {
    const s = run([{ type: "turn.finished", data: { turn_id: "t", status: "done", message_id: "m2" } }]);
    expect(pruneLive(s, new Set(["m1"]))).toBe(s);
    expect(pruneLive(s, new Set(["m1", "m2"]))).toEqual({});
    const failed = run([{ type: "turn.finished", data: { turn_id: "f", status: "failed", message_id: null } }]);
    expect(pruneLive(failed, new Set())).toEqual({});
  });

  it("names tools in words", () => {
    expect(toolLabel("tasks_create")).toBe("Create task");
    expect(toolLabel("memory.recall")).toBe("Recall memory");
    expect(toolLabel("entertainment_items_update")).toBe("Update entertainment item");
    expect(toolLabel("activities_list")).toBe("List activity");
  });
});
