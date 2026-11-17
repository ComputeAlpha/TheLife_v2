import { layoutColumn } from "./layout";

const s = (item: string, start: number, end: number) => ({ item, start, end });
const byItem = <T extends { item: string }>(xs: T[]) => Object.fromEntries(xs.map((x) => [x.item, x]));

describe("layoutColumn", () => {
  it("keeps separate entries full width", () => {
    const r = byItem(layoutColumn([s("a", 60, 120), s("b", 120, 180)]));
    expect(r.a).toMatchObject({ col: 0, cols: 1 });
    expect(r.b).toMatchObject({ col: 0, cols: 1 });
  });
  it("splits overlapping entries into columns", () => {
    const r = byItem(layoutColumn([s("a", 60, 180), s("b", 90, 150), s("c", 120, 240)]));
    expect(r.a).toMatchObject({ col: 0, cols: 3 });
    expect(r.b).toMatchObject({ col: 1, cols: 3 });
    expect(r.c).toMatchObject({ col: 2, cols: 3 });
  });
  it("reuses freed columns inside a cluster and widens into free space", () => {
    const r = byItem(layoutColumn([s("a", 0, 240), s("b", 30, 60), s("c", 90, 120)]));
    expect(r.b!.col).toBe(1);
    expect(r.c!.col).toBe(1); // b's column is free again
    expect(r.a).toMatchObject({ cols: 2, span: 1 });
    const w = byItem(layoutColumn([s("a", 0, 60), s("b", 0, 30), s("c", 0, 30), s("d", 40, 60)]));
    expect(w.d).toMatchObject({ col: 1, cols: 3, span: 2 }); // b and c have ended; d widens over two columns
  });
  it("gives point entries (a task due at 10:00) a visible height and clips to the day", () => {
    const r = byItem(layoutColumn([s("due", 600, 600), s("late", 1400, 1500), s("early", -60, 30)]));
    expect(r.due).toMatchObject({ start: 600, end: 620 });
    expect(r.late!.end).toBe(1440);
    expect(r.early!.start).toBe(0);
  });
});
