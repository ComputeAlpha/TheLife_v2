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
