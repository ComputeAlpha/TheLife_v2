import { paletteFilter } from "./CommandPalette";

describe("paletteFilter", () => {
  it("ranks page matches above the ask action", () => {
    expect(paletteFilter("go Settings", "settings")).toBeGreaterThan(paletteFilter("ask settings", "settings"));
    expect(paletteFilter("go Calendar", "cal")).toBe(1);
    expect(paletteFilter("go Goals & plans", "plans")).toBe(0.8);
    expect(paletteFilter("go Entertainment", "etmt")).toBe(0.3);
    expect(paletteFilter("go Chat", "xyz")).toBe(0);
  });
});

describe("paletteFilter search hits", () => {
  it("matches titles only, below page jumps", () => {
    expect(paletteFilter("find task 0b1c-ab", "ship", ["Ship the release"])).toBe(0.7);
    expect(paletteFilter("find task 0b1c-ab", "release", ["Ship the release"])).toBe(0.5);
    expect(paletteFilter("find task 0b1c-ab", "0b1c", ["Ship the release"])).toBe(0); // never the id
    expect(paletteFilter("find task 0b1c-ab", "stre", ["Ship the release"])).toBe(0); // no in-order letters
    expect(paletteFilter("go Tasks", "tas")).toBeGreaterThan(paletteFilter("find task x", "tas", ["Task list cleanup"]));
  });
});
