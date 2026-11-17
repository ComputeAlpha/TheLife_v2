import { fields, plainSummary } from "./ApprovalCard";

describe("approval card text", () => {
  it("keeps the readable part of the summary", () => {
    expect(plainSummary("Delete a task (soft delete). Target: Task 01a1 (version 1). Arguments: {}")).toBe("Delete a task (soft delete).");
    expect(plainSummary("Move all of Tuesday")).toBe("Move all of Tuesday");
  });
  it("drops ids and bookkeeping from the arguments", () => {
    expect(fields({ id: "01a1e587-740f-7000-878a-aee9c9ebd340", expected_version: 2, title: "Read", due_at: "2026-11-17T10:00:00Z" }))
      .toEqual([["title", "Read"], ["due at", "2026-11-17T10:00:00Z"]]);
  });
});
